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
    // The artifact must be bound to the signed hash (incl. an integer-valued f32 confidence of 1.0).
    assert.ok(report.checks.some((c) => /artifact bound/i.test(c)), 'artifact-binding check should run');
  });

  it('rejects a tampered artifact even when the digest is recomputed (P1)', async () => {
    const envelope = loadFixture('valid_envelope.json');
    // Doctor the human-readable artifact (swap a zone) and refresh the digest so the only thing
    // that can catch it is the artifact-to-signed-hash binding.
    envelope.artifact.batches[0].buckets[0].events[0].zone_id = 'zone:forged';
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    assert.match(report.error, /artifact hash mismatch/);
  });

  it('verifies the legacy (pre-auth_mode) envelope — old bundles stay valid forever', async () => {
    const envelope = loadFixture('valid_envelope_legacy.json');
    const digest = await V.computeWholeEnvelopeDigest(envelope);
    assert.equal(digest, envelope.whole_envelope_digest, 'cross-language digest mismatch');

    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, true, report.error);
    assert.equal(report.sealed_events, 3);
    assert.equal(report.export_receipts, 1);
    assert.equal(envelope.export_receipt_entry.receipt.auth_mode, undefined);
  });

  it('verifies the owner self-export envelope (auth_mode + disclosure window on the receipt)', async () => {
    const envelope = loadFixture('valid_envelope_self_export.json');
    const digest = await V.computeWholeEnvelopeDigest(envelope);
    assert.equal(digest, envelope.whole_envelope_digest, 'cross-language digest mismatch');

    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, true, report.error);
    assert.equal(report.sealed_events, 3);
    assert.equal(report.export_receipts, 2);
    assert.equal(envelope.export_receipt_entry.receipt.auth_mode, 'self_export');
    assert.deepEqual(envelope.export_receipt_entry.receipt.window,
      { start_epoch_s: 600, end_epoch_s: 2400 });
  });

  it('rejects a tampered payload (chain break)', async () => {
    const report = await V.verifyEnvelope(loadFixture('tampered_payload.json'));
    assert.equal(report.ok, false);
    assert.match(report.error, /sealed_events/);
    // Structured-failure parity pin: tests/envelope_fixtures.rs asserts this
    // exact triple for the same fixture via VerifyFailure.
    assert.deepEqual(report.failure,
      { ledger: 'sealed_events', entry_id: 0, kind: 'entry_hash_mismatch' });
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

describe('runtime without Ed25519 support', () => {
  // ed25519Supported() caches its probe per module instance, so each simulated-unsupported test
  // loads a fresh module that probes under the importKey stub. Returns { V, restore }.
  function withoutEd25519() {
    const realImport = globalThis.crypto.subtle.importKey.bind(globalThis.crypto.subtle);
    // Simulate an engine (Safari < 17, older Firefox) with Web Crypto but no Ed25519.
    globalThis.crypto.subtle.importKey = (fmt, key, algo, ext, usages) =>
      (algo && algo.name === 'Ed25519')
        ? Promise.reject(new Error('Ed25519 unsupported (simulated)'))
        : realImport(fmt, key, algo, ext, usages);
    delete require.cache[require.resolve('./verify_core')];
    const freshV = require('./verify_core');
    return { V: freshV, restore: () => {
      globalThis.crypto.subtle.importKey = realImport;
      delete require.cache[require.resolve('./verify_core')];
    } };
  }

  it('confirms Ed25519 IS available in this test runtime', async () => {
    assert.equal(await V.ed25519Supported(), true);
  });

  it('reports "inconclusive" (never "compromised") for valid evidence when Ed25519 is unavailable', async () => {
    const { V: noEd, restore } = withoutEd25519();
    try {
      const report = await noEd.verifyEnvelope(loadFixture('valid_envelope.json'));
      assert.equal(report.status, 'inconclusive');
      assert.equal(report.ok, false);
      assert.equal(report.inconclusive, true);
      assert.match(report.error, /cannot check Ed25519 signatures/i);
      // The deterministic checks must ALL have run — incl. the artifact-hash binding (Codex #1).
      assert.ok(report.checks.some((c) => /fingerprint recomputed/i.test(c)), 'fingerprint check should run');
      assert.ok(report.checks.some((c) => /artifact bound/i.test(c)), 'artifact-binding check should run');
      // ...but NOT claim any signature was verified.
      assert.ok(!report.checks.some((c) => /signature/i.test(c)), 'no signature check should be claimed');
    } finally {
      restore();
    }
  });

  it('still REJECTS a tampered artifact (recomputed digest) without Ed25519 — definitive, not inconclusive (Codex #1)', async () => {
    const { V: noEd, restore } = withoutEd25519();
    try {
      // Doctor the human-readable artifact and refresh the digest so only the artifact-to-signed-hash
      // binding (a SHA-256 check, no Ed25519) can catch it. This must NOT degrade to "inconclusive".
      const envelope = loadFixture('valid_envelope.json');
      envelope.artifact.batches[0].buckets[0].events[0].zone_id = 'zone:forged';
      envelope.whole_envelope_digest = await noEd.computeWholeEnvelopeDigest(envelope);
      const report = await noEd.verifyEnvelope(envelope);
      assert.equal(report.status, 'compromised');
      assert.equal(report.ok, false);
      assert.match(report.error, /artifact hash mismatch/);
    } finally {
      restore();
    }
  });

  it('still REJECTS a tampered digest without Ed25519 (caught before the signature stage)', async () => {
    const { V: noEd, restore } = withoutEd25519();
    try {
      const report = await noEd.verifyEnvelope(loadFixture('tampered_digest.json'));
      assert.equal(report.status, 'compromised');
      assert.equal(report.ok, false);
      assert.match(report.error, /whole_envelope_digest/);
    } finally {
      restore();
    }
  });
});
