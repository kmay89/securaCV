'use strict';

// Cross-language parity tests for the offline evidence-envelope verifier.
// These run the JS verifier against the SAME fixtures the Rust verifier checks
// (tests/fixtures/envelope/), proving both produce identical results & digests.
//
//   node --test viewer/verify_core.test.js

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const V = require('./verify_core');

const FIXTURES = path.join(__dirname, '..', 'tests', 'fixtures', 'envelope');
const loadFixture = (name) => JSON.parse(fs.readFileSync(path.join(FIXTURES, name), 'utf8'));

describe('canonical JSON parity', () => {
  it('sorts keys, omits whitespace, rejects floats and unsafe integers', () => {
    assert.equal(V.canonicalize({ b: 1, a: 2 }), '{"a":2,"b":1}');
    assert.equal(V.canonicalize({ z: { y: 1, x: 2 }, a: [3, 2, 1] }), '{"a":[3,2,1],"z":{"x":2,"y":1}}');
    assert.throws(() => V.canonicalize({ confidence: 0.85 }), /floating-point/);
    assert.throws(() => V.canonicalize({ n: 9007199254740992 }), /safe-integer/);
  });
});

describe('domain separation vectors', () => {
  it('matches the Rust domain_separated_hash for every vector', async () => {
    const { vectors } = loadFixture('domain_separation_vectors.json');
    assert.ok(vectors.length > 0);
    for (const v of vectors) {
      const got = V.bytesToHex(await V.domainSeparatedHash(v.domain, V.hexToBytes(v.entry_hash)));
      assert.equal(got, v.signing_hash, `domain=${v.domain} entry_hash=${v.entry_hash}`);
    }
  });
});

describe('envelope verification parity', () => {
  it('verifies the valid envelope and reproduces the stored digest', async () => {
    const envelope = loadFixture('valid_envelope.json');
    // The digest the JS verifier computes must equal the one Rust embedded.
    const digest = await V.computeWholeEnvelopeDigest(envelope);
    assert.equal(digest, envelope.whole_envelope_digest, 'cross-language digest mismatch');

    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, true, report.error);
    assert.equal(report.status, 'valid_with_warnings'); // PQ-not-checked warning
    assert.equal(report.sealed_events, 3);
    assert.equal(report.export_receipts, 1);
  });

  it('rejects a tampered payload (chain break)', async () => {
    const report = await V.verifyEnvelope(loadFixture('tampered_payload.json'));
    assert.equal(report.ok, false);
    assert.match(report.error, /sealed_events/);
  });

  it('rejects a corrupted digest', async () => {
    const report = await V.verifyEnvelope(loadFixture('tampered_digest.json'));
    assert.equal(report.ok, false);
    assert.match(report.error, /whole_envelope_digest/);
  });

  it('rejects a forged ledger count (summary not signed)', async () => {
    const envelope = loadFixture('valid_envelope.json');
    envelope.ledgers.sealed_events.count = 99;
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    assert.match(report.error, /sealed_events count mismatch/);
  });
});
