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

  it('verifies an envelope from a rotated device — the ledgers mix signers', async () => {
    const envelope = loadFixture('valid_envelope_rotated.json');
    const digest = await V.computeWholeEnvelopeDigest(envelope);
    assert.equal(digest, envelope.whole_envelope_digest, 'cross-language digest mismatch');

    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, true, report.error);
    assert.equal(report.sealed_events, 3); // pre-event, rotation record, post-event
    assert.ok(report.checks.some((c) => /rotation/i.test(c)),
      'the report should say the rotation lineage was followed');
  });

  it('rejects a rotated envelope whose rotation attestation is tampered', async () => {
    const envelope = loadFixture('valid_envelope_rotated.json');
    const rotIdx = envelope.ledgers.sealed_events.entries.findIndex(
      (e) => e.payload_json.includes('key_rotation'));
    assert.ok(rotIdx >= 0, 'fixture must carry the rotation record');
    const payload = JSON.parse(envelope.ledgers.sealed_events.entries[rotIdx].payload_json);
    payload.new_key_attestation[0] ^= 0xff;
    envelope.ledgers.sealed_events.entries[rotIdx].payload_json = JSON.stringify(payload);
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    // The lineage check names the rotation; a dodged variant would break the
    // row's entry hash instead — rejected either way.
    assert.match(report.error, /rotation|hash/i);
  });

  it('rejects an envelope whose export-receipts ledger head is not its own receipt', async () => {
    // The export that seals an envelope appends its receipt LAST, so the
    // export_receipt_entry must be the presented ledger's head — otherwise a
    // retired-key holder could substitute a receipt ledger. Drop the only
    // ledger row (an empty chain still hash-verifies trivially) and recompute
    // the digest: only the head binding can catch it.
    const envelope = loadFixture('valid_envelope.json');
    envelope.ledgers.export_receipts.entries = [];
    envelope.ledgers.export_receipts.head_hash = null;
    envelope.ledgers.export_receipts.count = 0;
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    assert.match(report.error, /head does not match/);
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

describe('export-bundle verification parity', () => {
  const BUNDLE_FIXTURES = path.join(__dirname, '..', 'tests', 'fixtures', 'export_bundle');
  const loadBundle = () => JSON.parse(fs.readFileSync(path.join(BUNDLE_FIXTURES, 'valid_bundle.json'), 'utf8'));

  it('recognizes the two input kinds', () => {
    assert.equal(V.looksLikeExportBundle(loadBundle()), true);
    assert.equal(V.looksLikeEnvelope(loadBundle()), false);
    const envelope = loadFixture('valid_envelope.json');
    assert.equal(V.looksLikeEnvelope(envelope), true);
    assert.equal(V.looksLikeExportBundle(envelope), false);
  });

  it('verifies the fixture the Rust verifier pins (tests/export_bundle_fixtures.rs)', async () => {
    const report = await V.verifyExportBundle(loadBundle());
    assert.equal(report.ok, true, report.error);
    // Self-attested authorship: valid, with the key-provenance note.
    assert.equal(report.status, 'valid_with_warnings');
    assert.equal(report.authorship, 'bundle');
    assert.ok(report.warnings.some((w) => /self-attested/.test(w)));
    assert.equal(report.events, 3);
    assert.equal(report.failures, 0);
    assert.ok(report.checks.some((c) => /artifact bound/i.test(c)));
  });

  it('confirms authorship when the matching trusted key is supplied', async () => {
    const bundle = loadBundle();
    const keyHex = bundle.device_public_key.map((b) => b.toString(16).padStart(2, '0')).join('');
    const report = await V.verifyExportBundle(bundle, { trustedDeviceKeyHex: keyHex });
    assert.equal(report.ok, true, report.error);
    assert.equal(report.authorship, 'trusted');
    assert.equal(report.status, 'ok');
    assert.ok(!report.warnings.some((w) => /self-attested/.test(w)));
  });

  it('rejects a non-matching trusted key', async () => {
    const bundle = loadBundle();
    const report = await V.verifyExportBundle(bundle, { trustedDeviceKeyHex: '00'.repeat(32) });
    assert.equal(report.ok, false);
    assert.match(report.error, /device key mismatch/);
  });

  it('rejects a tampered artifact (mirrors the Rust tampered_artifact test)', async () => {
    const bundle = loadBundle();
    bundle.artifact.batches[0].buckets[0].events[0].zone_id = 'zone:forged';
    const report = await V.verifyExportBundle(bundle);
    assert.equal(report.ok, false);
    assert.match(report.error, /artifact hash mismatch/);
  });

  it('rejects a tampered receipt (mirrors the Rust tampered_receipt test)', async () => {
    const bundle = loadBundle();
    bundle.receipt_entry.receipt.batch_size += 1;
    const report = await V.verifyExportBundle(bundle);
    assert.equal(report.ok, false);
    assert.match(report.error, /entry_hash mismatch/);
  });

  it('rejects a swapped device key at the signature check', async () => {
    const bundle = loadBundle();
    bundle.device_public_key[0] ^= 0x01;
    const report = await V.verifyExportBundle(bundle);
    assert.equal(report.ok, false);
  });

  it('is inconclusive — never rejected — without Ed25519, but still catches tamper', async () => {
    const realImport = globalThis.crypto.subtle.importKey.bind(globalThis.crypto.subtle);
    globalThis.crypto.subtle.importKey = (fmt, key, algo, ext, usages) =>
      (algo && algo.name === 'Ed25519')
        ? Promise.reject(new Error('Ed25519 unsupported (simulated)'))
        : realImport(fmt, key, algo, ext, usages);
    delete require.cache[require.resolve('./verify_core')];
    const noEd = require('./verify_core');
    try {
      const clean = await noEd.verifyExportBundle(loadBundle());
      assert.equal(clean.status, 'inconclusive');
      assert.equal(clean.ok, false);
      assert.ok(clean.checks.some((c) => /artifact bound/i.test(c)), 'artifact binding must still run');
      assert.ok(!clean.checks.some((c) => /signature/i.test(c)), 'no signature check should be claimed');

      const tampered = loadBundle();
      tampered.artifact.batches[0].buckets[0].events[0].confidence = 0.5;
      const report = await noEd.verifyExportBundle(tampered);
      assert.equal(report.status, 'compromised');
      assert.match(report.error, /artifact hash mismatch/);
    } finally {
      globalThis.crypto.subtle.importKey = realImport;
      delete require.cache[require.resolve('./verify_core')];
    }
  });
});
