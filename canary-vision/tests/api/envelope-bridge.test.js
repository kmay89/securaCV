'use strict';

// Tests for the evidence-envelope bridge: it must coarsen the raw witness chain into a canonical
// securacv-evidence-envelope that the shared verifier (viewer/verify_core.js) accepts, while
// stripping every forbidden field (precise timestamps, GPS, thumbnails). Privacy and verifiability
// are the whole point, so both are asserted directly.

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const path = require('node:path');

const { buildEvidenceEnvelope, EnvelopeBridgeError } = require('../../device-api/lib/envelope-bridge');
const { computeHash, signHash } = require('../../device-api/lib/witness-chain');
const V = require(path.join(__dirname, '..', '..', '..', 'viewer', 'verify_core.js'));
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient } = require('../helpers/test-client');

function makeKeys() { return crypto.generateKeyPairSync('ed25519'); }
const DEVICE = { device_id: 'canary-a3f7', name: 'Front Porch', firmware_version: '0.4.1' };
const T0 = Date.parse('2026-05-29T12:34:56.789Z');

// Mint a properly hash-chained, Ed25519-signed raw witness chain (mirrors device-state.js's
// addWitnessRecord) so the bridge's source-chain validation accepts it. Each spec is
// { zone, event_type, timestamp, extra? } — `extra` adds precise fields the bridge must strip.
function signedChain(privateKey, specs) {
  const records = [];
  let prevHash = '0'.repeat(64);
  for (let i = 0; i < specs.length; i++) {
    const s = specs[i];
    const timeSource = s.extra && s.extra.gps_timestamp ? 'gps_utc' : 'device_clock';
    const gpsTimestamp = (s.extra && s.extra.gps_timestamp) || '';
    const hash = computeHash(i + 1, prevHash, s.timestamp, s.event_type, s.zone, timeSource, gpsTimestamp);
    const record = {
      seq: i + 1, hash, prev_hash: prevHash, timestamp: s.timestamp,
      event_type: s.event_type, zone: s.zone, signature: signHash(hash, privateKey),
      time_source: timeSource, ...(s.extra || {}),
    };
    records.push(record);
    prevHash = hash;
  }
  return records;
}

// Realistic specs: carry precise data exactly as the device produces it (thumbnail + GPS).
function rawSpecs() {
  return [
    { zone: 'front', event_type: 'person_detected', timestamp: new Date(T0).toISOString(),
      extra: { thumbnail: 'data:image/x-portable-graymap;base64,AAAA', gps_timestamp: '2026-05-29T12:34:56Z',
        gps_fix_quality: 'good', gps_satellites: 9, gps_fix_age_ms: 1200 } },
    { zone: 'drive', event_type: 'vehicle_detected', timestamp: new Date(T0 + 5000).toISOString(),
      extra: { thumbnail: 'data:image/x-portable-graymap;base64,BBBB' } },
    { zone: 'front', event_type: 'motion_detected', timestamp: new Date(T0 + 700000).toISOString(),
      extra: { thumbnail: 'data:image/x-portable-graymap;base64,CCCC' } },
  ];
}

function rawRecords(privateKey) { return signedChain(privateKey, rawSpecs()); }

describe('envelope bridge — construction & verification', () => {
  it('produces an envelope the shared verifier accepts', async () => {
    const { publicKey, privateKey } = makeKeys();
    const env = await buildEvidenceEnvelope({ records: rawRecords(privateKey), publicKey, privateKey, device: DEVICE, nowMs: T0 });
    const report = await V.verifyEnvelope(env);
    assert.equal(report.ok, true, report.error);
    assert.equal(report.sealed_events, 3);
    assert.equal(report.export_receipts, 1);
    assert.equal(env.envelope_format, 'securacv-evidence-envelope');
    assert.equal(env.envelope_version, 1);
  });

  it('strips every forbidden field (privacy coarsening)', async () => {
    const { publicKey, privateKey } = makeKeys();
    const env = await buildEvidenceEnvelope({ records: rawRecords(privateKey), publicKey, privateKey, device: DEVICE, nowMs: T0 });
    const s = JSON.stringify(env);
    assert.ok(!s.includes('thumbnail'), 'thumbnail must not survive');
    assert.ok(!s.includes('gps_'), 'GPS fields must not survive');
    assert.ok(!s.includes('time_source'), 'time_source must not survive');
    assert.ok(!/\d{2}:\d{2}:\d{2}\.\d{3}Z/.test(s), 'millisecond timestamps must not survive');
  });

  it('buckets timestamps to a coarse grid (default 600s)', async () => {
    const { publicKey, privateKey } = makeKeys();
    const env = await buildEvidenceEnvelope({ records: rawRecords(privateKey), publicKey, privateKey, device: DEVICE, nowMs: T0 });
    const buckets = env.artifact.batches[0].buckets;
    // seq 1 & 2 are 5s apart -> same bucket; seq 3 is ~11.6 min later -> next bucket.
    assert.equal(buckets.length, 2);
    for (const b of buckets) {
      assert.equal(b.time_bucket.size_s, 600);
      assert.equal(b.time_bucket.start_epoch_s % 600, 0, 'bucket start must be aligned to the grid');
    }
  });

  it('rejects an unmappable event type', async () => {
    const { publicKey, privateKey } = makeKeys();
    const records = signedChain(privateKey, [
      { zone: 'front', event_type: 'alien_landing', timestamp: new Date(T0).toISOString() }]);
    await assert.rejects(
      buildEvidenceEnvelope({ records, publicKey, privateKey, device: DEVICE }),
      EnvelopeBridgeError,
    );
  });

  it('rejects an unparseable timestamp', async () => {
    const { publicKey, privateKey } = makeKeys();
    const records = signedChain(privateKey, [
      { zone: 'front', event_type: 'person_detected', timestamp: 'not-a-date' }]);
    await assert.rejects(
      buildEvidenceEnvelope({ records, publicKey, privateKey, device: DEVICE }),
      EnvelopeBridgeError,
    );
  });

  it('refuses a free-text zone id (privacy: no addresses/names)', async () => {
    const { publicKey, privateKey } = makeKeys();
    const records = signedChain(privateKey, [
      { zone: '123 Main St, Apt 4', event_type: 'person_detected', timestamp: new Date(T0).toISOString() }]);
    await assert.rejects(
      buildEvidenceEnvelope({ records, publicKey, privateKey, device: DEVICE }),
      /non-local zone id/,
    );
  });

  it('refuses a tampered source chain (no laundering)', async () => {
    const { publicKey, privateKey } = makeKeys();
    const records = rawRecords(privateKey);
    records[1].zone = 'back'; // mutate after signing -> hash no longer matches
    await assert.rejects(
      buildEvidenceEnvelope({ records, publicKey, privateKey, device: DEVICE }),
      /raw chain broken/,
    );
  });

  it('refuses a source chain with a sequence gap', async () => {
    const { publicKey, privateKey } = makeKeys();
    const records = rawRecords(privateKey);
    records.splice(1, 1); // drop seq 2 -> gap (and prev_hash break)
    await assert.rejects(
      buildEvidenceEnvelope({ records, publicKey, privateKey, device: DEVICE }),
      /raw chain/,
    );
  });

  it('refuses a source chain signed by a different key', async () => {
    const { publicKey } = makeKeys();
    const other = makeKeys();
    const records = rawRecords(other.privateKey); // valid chain, wrong signer
    await assert.rejects(
      buildEvidenceEnvelope({ records, publicKey, privateKey: other.privateKey, device: DEVICE }),
      /invalid device signature/,
    );
  });

  it('handles an empty chain', async () => {
    const { publicKey, privateKey } = makeKeys();
    const env = await buildEvidenceEnvelope({ records: [], publicKey, privateKey, device: DEVICE, nowMs: T0 });
    const report = await V.verifyEnvelope(env);
    assert.equal(report.ok, true, report.error);
    assert.equal(report.sealed_events, 0);
  });

  it('rejects a tampered envelope (sealed event mutated after signing)', async () => {
    const { publicKey, privateKey } = makeKeys();
    const env = await buildEvidenceEnvelope({ records: rawRecords(privateKey), publicKey, privateKey, device: DEVICE, nowMs: T0 });
    env.ledgers.sealed_events.entries[0].payload_json =
      env.ledgers.sealed_events.entries[0].payload_json.replace('zone:a', 'zone:x').replace('front', 'forged');
    env.whole_envelope_digest = await V.computeWholeEnvelopeDigest(env);
    const report = await V.verifyEnvelope(env);
    assert.equal(report.ok, false);
  });
});

describe('GET /api/v1/witness/envelope', () => {
  let server, client;
  before(async () => { server = await startServer({ devMode: true }); client = createClient(server.url, TEST_TOKEN); });
  after(async () => { await server.close(); });

  it('returns a verifiable canonical envelope from the live device chain', async () => {
    const res = await client.get('/api/v1/witness/envelope');
    assert.equal(res.status, 200);
    assert.equal(res.json.envelope_format, 'securacv-evidence-envelope');
    const report = await V.verifyEnvelope(res.json);
    assert.equal(report.ok, true, report.error);
    // The seeded device chain carries thumbnails; the endpoint must have stripped them.
    const s = JSON.stringify(res.json);
    assert.ok(!s.includes('thumbnail') && !s.includes('gps_'), 'endpoint must strip precise data');
  });
});

describe('POST /api/v1/witness/verify — shared-verifier reuse', () => {
  let server, client;
  before(async () => { server = await startServer({ devMode: true }); client = createClient(server.url, TEST_TOKEN); });
  after(async () => { await server.close(); });

  it('includes an evidence_envelope report from the shared verifier', async () => {
    const res = await client.post('/api/v1/witness/verify');
    assert.equal(res.status, 200);
    assert.ok(res.json.evidence_envelope, 'verify response should carry the envelope report');
    assert.equal(res.json.evidence_envelope.ok, true, res.json.evidence_envelope.error);
    assert.ok(Array.isArray(res.json.evidence_envelope.checks));
    assert.ok(res.json.evidence_envelope.whole_envelope_digest);
  });
});
