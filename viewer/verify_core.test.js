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

  // Without a trusted key the envelope's own provenance key is the only
  // thing the signatures were checked against — the verdict is self-attested
  // authorship, and the result must say so, exactly as verifyExportBundle does.
  it('without a trusted key, authorship is self-attested and the report says so', async () => {
    const report = await V.verifyEnvelope(loadFixture('valid_envelope.json'));
    assert.equal(report.ok, true, report.error);
    assert.equal(report.authorship, 'envelope');
    assert.equal(report.device_public_key, loadFixture('valid_envelope.json').provenance.device_public_key);
    assert.ok(report.warnings.some((w) => /self-attested/.test(w)), 'the self-attested note must be present');
    assert.ok(report.checks.some((c) => /under the device key named in the envelope/.test(c)));
  });

  it('confirms authorship when the matching trusted key is supplied', async () => {
    const envelope = loadFixture('valid_envelope.json');
    const report = await V.verifyEnvelope(envelope, { trustedDeviceKeyHex: envelope.provenance.device_public_key.toUpperCase() });
    assert.equal(report.ok, true, report.error);
    assert.equal(report.authorship, 'trusted');
    assert.ok(!report.warnings.some((w) => /self-attested/.test(w)));
    assert.ok(report.checks.some((c) => /matches the trusted key you supplied/.test(c)));
    assert.ok(report.checks.some((c) => /authorship confirmed against your trusted key/.test(c)));
  });

  it('rejects a non-matching trusted key before any signature is checked', async () => {
    const report = await V.verifyEnvelope(loadFixture('valid_envelope.json'), { trustedDeviceKeyHex: '00'.repeat(32) });
    assert.equal(report.ok, false);
    assert.equal(report.status, 'compromised');
    assert.match(report.error, /device key mismatch/);
    assert.ok(!report.checks.some((c) => /signatures valid/.test(c)), 'no signature verdict may be reached under a key the caller did not trust');
  });

  it('refuses a malformed trusted key rather than ignoring it', async () => {
    for (const bad of ['zz'.repeat(32), 'abcd']) {
      const report = await V.verifyEnvelope(loadFixture('valid_envelope.json'), { trustedDeviceKeyHex: bad });
      assert.equal(report.ok, false, `accepted trusted key ${bad}`);
      assert.match(report.error, /supplied device key/);
    }
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

  it('rejects a gaps.failure_count the signed artifact does not carry', async () => {
    const envelope = loadFixture('valid_envelope.json');
    // gaps is not signature-bound; the artifact it is derived from is. Editing the count and
    // refreshing the digest must still be caught by re-derivation (mirrors src/envelope.rs).
    envelope.gaps.failure_count = envelope.gaps.failure_count + 1;
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    assert.match(report.error, /gaps\.failure_count .* does not match/);
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

  // A Granted row no trustee approved: device-signed, commitment matching, the
  // approval's signature filler. Same fixture and warning text as
  // tests/envelope_fixtures.rs.
  it('reports a break-glass grant as present, not verified — a forged grant is never ok', async () => {
    const WARNING = '1 break-glass grant(s) present: trustee approvals are not verified offline';
    const envelope = loadFixture('forged_grant_envelope.json');
    const approvals = JSON.parse(envelope.ledgers.break_glass_receipts.entries[0].approvals_json);
    assert.deepEqual(approvals[0].signature, new Array(64).fill(0xaa), 'filler signature');

    const trusted = { trustedDeviceKeyHex: envelope.provenance.device_public_key };
    for (const opts of [{}, trusted]) {
      const report = await V.verifyEnvelope(envelope, opts);
      assert.equal(report.ok, true, report.error);
      assert.equal(report.status, 'valid_with_warnings');
      assert.equal(report.break_glass_granted, 1);
      assert.ok(report.warnings.includes(WARNING), report.warnings.join(' | '));
      assert.ok(!report.checks.some((c) => /break-glass.*verified/i.test(c)), 'no check may call the receipts verified');
    }

    // The grant alone causes the warning: without the row, every other warning stays.
    const withGrant = (await V.verifyEnvelope(envelope, trusted)).warnings;
    const bg = envelope.ledgers.break_glass_receipts;
    bg.entries = []; bg.count = 0; bg.head_hash = null;
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const bare = await V.verifyEnvelope(envelope, trusted);
    assert.equal(bare.ok, true, bare.error);
    assert.deepEqual(withGrant, [WARNING, ...bare.warnings]);
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
  // The bundle a paired bearer (the Home Assistant integration's credential)
  // downloads: receipt `auth_mode: paired`, a disclosure window, and the
  // trailing `token_id` — every optional receipt field populated.
  const loadPairedBundle = () => JSON.parse(fs.readFileSync(path.join(BUNDLE_FIXTURES, 'valid_bundle_paired.json'), 'utf8'));

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

  it('verifies the paired-bearer fixture the Rust verifier pins (auth_mode + window + token_id on the receipt)', async () => {
    const bundle = loadPairedBundle();
    assert.equal(bundle.receipt_entry.receipt.auth_mode, 'paired');
    assert.equal(bundle.receipt_entry.receipt.token_id, '0badf00d');
    assert.ok(bundle.receipt_entry.receipt.window, 'the paired export was windowed');
    const report = await V.verifyExportBundle(loadPairedBundle());
    assert.equal(report.ok, true, report.error);
    assert.equal(report.events, 3);
    assert.ok(report.checks.some((c) => /artifact bound/i.test(c)));
    // The serializer puts token_id LAST, after window — the order the kernel signed.
    const keyHex = bundle.device_public_key.map((b) => b.toString(16).padStart(2, '0')).join('');
    const trusted = await V.verifyExportBundle(bundle, { trustedDeviceKeyHex: keyHex });
    assert.equal(trusted.ok, true, trusted.error);
    assert.equal(trusted.authorship, 'trusted');
  });

  it('rejects a paired bundle whose bearer id is edited or scrubbed (token_id is signed)', async () => {
    const edited = loadPairedBundle();
    edited.receipt_entry.receipt.token_id = 'deadbeef';
    const report = await V.verifyExportBundle(edited);
    assert.equal(report.ok, false);
    assert.match(report.error, /entry_hash mismatch/);

    const scrubbed = loadPairedBundle();
    delete scrubbed.receipt_entry.receipt.token_id;
    const report2 = await V.verifyExportBundle(scrubbed);
    assert.equal(report2.ok, false);
    assert.match(report2.error, /entry_hash mismatch/);
  });

  it('treats an unknown auth_mode as an opaque label (a new credential class never breaks an old viewer)', async () => {
    // Re-labeling a receipt breaks its signed hash (that is tamper), but the
    // serializer itself must accept any string: it never interprets the mode.
    const bundle = loadPairedBundle();
    bundle.receipt_entry.receipt.auth_mode = 'some_future_mode';
    const report = await V.verifyExportBundle(bundle);
    assert.equal(report.ok, false);
    assert.match(report.error, /entry_hash mismatch/, 'rejected by the hash, not by a vocabulary check');
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

// ---- handoff-era receipts (ExportReceipt.context / authorizing_receipt, the Unsealed row) ----
// The Rust side pins the same literals in src/disclosure_tests.rs
// (export_receipt_generations_serialize_byte_identically) and the same fixtures in
// tests/export_bundle_fixtures.rs and tests/envelope_fixtures.rs. Regenerate the fixtures with
//   cargo run --example gen_handoff_fixtures
describe('handoff-era receipts: context, authorizing_receipt and the unseal record', () => {
  const nodeCrypto = require('node:crypto');
  const BUNDLE_FIXTURES = path.join(__dirname, '..', 'tests', 'fixtures', 'export_bundle');
  const loadNamed = (name) => JSON.parse(fs.readFileSync(path.join(BUNDLE_FIXTURES, name), 'utf8'));
  const rep = (b) => Array(32).fill(b);
  const ints = (b) => rep(b).join(',');
  const head = `{"time_bucket":{"start_epoch_s":600,"size_s":600},"ruleset_hash":[${ints(1)}],"batch_size":50,"artifact_hash":[${ints(2)}]`;
  const base = { time_bucket: { start_epoch_s: 600, size_s: 600 }, ruleset_hash: rep(1), batch_size: 50, artifact_hash: rep(2) };
  const win = { start_epoch_s: 600, end_epoch_s: 1200 };

  it('serializes the five receipt generations byte-identically to the Rust literals', () => {
    assert.equal(V.serExportReceipt(base), head + '}');
    assert.equal(V.serExportReceipt({ ...base, auth_mode: 'paired', window: win, token_id: 'deadbeef' }),
      head + ',"auth_mode":"paired","window":{"start_epoch_s":600,"end_epoch_s":1200},"token_id":"deadbeef"}');
    assert.equal(V.serExportReceipt({
      ...base, auth_mode: 'self_export',
      context: { operator: { requester_name: 'Dana Owner', reason_code: 'owner-recovery', case_ref: 'HOA-114', requester_key: null }, recipient_commitment: rep(3) },
    }), head + `,"auth_mode":"self_export","context":{"operator":{"requester_name":"Dana Owner","reason_code":"owner-recovery","case_ref":"HOA-114","requester_key":null},"recipient_commitment":[${ints(3)}]}}`);
    assert.equal(V.serExportReceipt({ ...base, auth_mode: 'break_glass', window: win, context: { recipient_commitment: rep(3) }, authorizing_receipt: rep(4) }),
      head + `,"auth_mode":"break_glass","window":{"start_epoch_s":600,"end_epoch_s":1200},"context":{"recipient_commitment":[${ints(3)}]},"authorizing_receipt":[${ints(4)}]}`);
    assert.equal(V.serExportReceipt({ ...base, auth_mode: 'break_glass', authorizing_receipt: rep(4) }),
      head + `,"auth_mode":"break_glass","authorizing_receipt":[${ints(4)}]}`);
  });

  it('refuses a malformed 32-byte array instead of serializing it', () => {
    assert.throws(() => V.serExportReceipt({ ...base, authorizing_receipt: rep(4).slice(1) }), /32-byte array/);
    assert.throws(() => V.serExportReceipt({ ...base, authorizing_receipt: [...rep(4).slice(1), 256] }), /out of range/);
  });

  it('verifies the self-export context and quorum fixtures, labeling what is declared', async () => {
    const self = await V.verifyExportBundle(loadNamed('valid_bundle_self_context.json'));
    assert.equal(self.ok, true, self.error);
    assert.ok(self.checks.some((c) => /^Declared operator: Dana Owner \(owner-recovery\) — declared by the exporting operator, not trustee-consented$/.test(c)));
    assert.ok(self.checks.includes('Recipient: committed in the signed receipt (named only in the handoff sidecar)'));
    const quorum = await V.verifyExportBundle(loadNamed('valid_bundle_quorum.json'));
    assert.equal(quorum.ok, true, quorum.error);
    assert.ok(quorum.checks.some((c) => /^Authorized by Granted break-glass receipt [0-9a-f]{16}… \(consent recorded on the grant\)$/.test(c)));
    assert.ok(!quorum.checks.some((c) => /trustee-consented/.test(c) && /Declared/.test(c)));
  });

  it('rejects an edited context, operator, commitment or authorizing_receipt by the entry hash', async () => {
    const edits = [
      ['valid_bundle_self_context.json', (r) => { r.context.operator.requester_name = 'Someone Else'; }],
      ['valid_bundle_self_context.json', (r) => { delete r.context; }],
      ['valid_bundle_self_context.json', (r) => { r.context.recipient_commitment[0] ^= 1; }],
      ['valid_bundle_quorum.json', (r) => { r.authorizing_receipt[0] ^= 1; }],
      ['valid_bundle_quorum.json', (r) => { delete r.authorizing_receipt; }],
    ];
    for (const [name, edit] of edits) {
      const b = loadNamed(name);
      edit(b.receipt_entry.receipt);
      const report = await V.verifyExportBundle(b);
      assert.equal(report.ok, false, name);
      assert.match(report.error, /entry_hash mismatch/);
    }
  });

  it('applies R0, R1 and R6 structurally with the Rust strings', () => {
    assert.throws(() => V.checkReceiptAuthorizationShape({ authorizing_receipt: [1, 2] }),
      { message: "export receipt's authorizing_receipt is not a 32-byte array" });
    assert.throws(() => V.checkReceiptAuthorizationShape({ auth_mode: 'self_export', authorizing_receipt: rep(4) }),
      { message: 'export receipt names an authorizing receipt but its auth_mode is self_export, not break_glass' });
    assert.throws(() => V.checkReceiptAuthorizationShape({ authorizing_receipt: rep(4) }),
      { message: 'export receipt names an authorizing receipt but its auth_mode is absent, not break_glass' });
    assert.throws(() => V.checkReceiptAuthorizationShape({ auth_mode: 'break_glass', authorizing_receipt: rep(4), context: { operator: { requester_name: 'x', reason_code: 'y' } } }),
      { message: "export receipt carries an operator context beside its authorizing receipt (a quorum export's operator is its grant's)" });
    assert.equal(V.checkReceiptAuthorizationShape({ auth_mode: 'self_export' }), null);
  });

  it('reads R0 from the signed payload text, refusing float-encoded bytes as Rust does', () => {
    // The same payload texts src/disclosure_tests.rs r0_refuses_a_float_encoded_authorizing_receipt pins.
    const r0 = "export receipt's authorizing_receipt is not a 32-byte array";
    for (const element of ['4.0', '4e0', '-0']) {
      const items = Array(32).fill('4');
      items[7] = element;
      const raw = `{"auth_mode":"break_glass","authorizing_receipt":[${items.join(',')}]}`;
      const parsed = JSON.parse(raw);
      // JSON.parse alone would pass it (4.0 and 4e0 become 4; -0 becomes 0)...
      assert.doesNotThrow(() => V.checkReceiptAuthorizationShape(parsed));
      // ...the ledger walk reads the signed text.
      assert.throws(() => V.checkRawAuthorizingReceipt(raw), { message: r0 }, element);
      assert.throws(() => V.checkExportAuthorizations(new Map(), new Set(), [[0, parsed, raw]]),
        (e) => e.message === r0 && e.index === 0, element);
    }
    const raw = `{"auth_mode":"break_glass","authorizing_receipt":[${Array(32).fill('4').join(',')}]}`;
    assert.throws(() => V.checkExportAuthorizations(new Map(), new Set(), [[0, JSON.parse(raw), raw]]),
      { message: `export receipt names authorizing receipt ${'04'.repeat(32)} but no Granted break-glass receipt has that hash` });
    // Only the top-level member counts; a same-named key nested elsewhere is not it.
    const nested = `{"context":{"authorizing_receipt":[4.0]},"auth_mode":"break_glass","authorizing_receipt":[${Array(32).fill('4').join(',')}]}`;
    V.checkRawAuthorizingReceipt(nested);
  });

  it('echoes a forged auth_mode through displaySafe, as Rust R1 does', () => {
    // The strings src/disclosure_tests.rs r1_sanitizes_a_forged_auth_mode pins.
    assert.throws(() => V.checkReceiptAuthorizationShape({ auth_mode: 'x\u001b[2J\nOK: chain valid\u202e', authorizing_receipt: rep(4) }),
      { message: 'export receipt names an authorizing receipt but its auth_mode is x\ufffd[2J\ufffdOK: chain valid\ufffd, not break_glass' });
    assert.throws(() => V.checkReceiptAuthorizationShape({ auth_mode: 1.0, authorizing_receipt: rep(4) }),
      { message: 'export receipt names an authorizing receipt but its auth_mode is a non-string value, not break_glass' });
  });

  it('applies R2-R5 across the ledgers with the Rust strings', () => {
    const g = '04'.repeat(32);
    const r = { auth_mode: 'break_glass', authorizing_receipt: rep(4) };
    assert.throws(() => V.checkExportAuthorizations(new Map(), new Set(), [[0, r]]),
      { message: `export receipt names authorizing receipt ${g} but no Granted break-glass receipt has that hash` });
    assert.throws(() => V.checkExportAuthorizations(new Map([[g, { envelope: 'vault:‮1' }]]), new Set([g]), [[0, r]]),
      { message: `export receipt's authorizing grant ${g} is for envelope 'vault:�1', not export:events` });
    assert.throws(() => V.checkExportAuthorizations(new Map([[g, { envelope: 'export:events' }]]), new Set(), [[0, r]]),
      { message: `export receipt's authorizing grant ${g} has no redemption row — its token was never spent` });
    assert.throws(() => V.checkExportAuthorizations(new Map([[g, { envelope: 'export:events' }]]), new Set([g]), [[0, r], [3, r]]),
      { message: `export receipts 0 and 3 both name authorizing grant ${g} — a grant authorizes one export` });
    assert.equal(V.checkExportAuthorizations(new Map([[g, { envelope: 'export:events' }]]), new Set([g]), [[0, r], [1, { auth_mode: 'api' }]]), 1);
  });

  it('never softens an entry-hash mismatch because of unknown receipt fields (the no-softening pin)', async () => {
    const b = loadNamed('valid_bundle_quorum.json');
    b.receipt_entry.receipt.some_future_field = 7;
    b.receipt_entry.receipt.context.some_future_member = 1;
    // A newer build signed bytes that include those fields; this verifier's
    // re-serialization omits them, so the entry hash cannot re-derive.
    b.receipt_entry.entry_hash[0] ^= 1;
    assert.deepEqual(V.unknownReceiptFields(b.receipt_entry.receipt), ['some_future_field', 'context.some_future_member']);
    const report = await V.verifyExportBundle(b);
    assert.equal(report.status, 'compromised');
    assert.equal(report.ok, false);
    assert.match(report.error, /^export receipt entry_hash mismatch \(this receipt also carries field\(s\) this verifier does not know: some_future_field, context\.some_future_member; if a newer SecuraCV build wrote it, re-check with that build — the mismatch stands until then\)$/);
  });

  it('verifies the unsealed envelope fixture and counts the unseal record and the authorized export', async () => {
    const report = await V.verifyEnvelope(loadFixture('valid_envelope_unsealed.json'));
    assert.equal(report.ok, true, report.error);
    assert.equal(report.break_glass_granted, 2);
    assert.equal(report.break_glass_redeemed, 2);
    assert.equal(report.break_glass_unsealed, 1);
    assert.equal(report.export_receipts_authorized, 1);
  });

  it('every pre-existing fixture still verifies, with zero new counts', async () => {
    for (const name of ['valid_envelope.json', 'valid_envelope_legacy.json', 'valid_envelope_rotated.json', 'valid_envelope_self_export.json']) {
      const report = await V.verifyEnvelope(loadFixture(name));
      assert.equal(report.ok, true, `${name}: ${report.error}`);
      assert.equal(report.break_glass_unsealed, 0);
      assert.equal(report.export_receipts_authorized, 0);
    }
    for (const name of ['valid_bundle.json', 'valid_bundle_paired.json']) {
      const report = await V.verifyExportBundle(loadNamed(name));
      assert.equal(report.ok, true, `${name}: ${report.error}`);
    }
  });

  // Re-sign forged rows with the fixture device key (SHA-256 of the fixture seed — a test
  // vector, never a production key) so the U and R rules, not the signature check, decide.
  const FIXTURE_SEED = 'devkey:fixture:0123456789abcdef0123';
  const devicePrivateKey = () => {
    const seed = nodeCrypto.createHash('sha256').update(FIXTURE_SEED).digest();
    const der = Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), seed]);
    return nodeCrypto.createPrivateKey({ key: der, format: 'der', type: 'pkcs8' });
  };
  async function signEntry(domain, entryHash) {
    const msg = await V.domainSeparatedHash(domain, entryHash);
    return Array.from(nodeCrypto.sign(null, Buffer.from(msg), devicePrivateKey()));
  }
  async function appendBreakGlassRow(envelope, receipt) {
    const bg = envelope.ledgers.break_glass_receipts;
    const prev = bg.entries[bg.entries.length - 1].entry_hash; // hex in the envelope ledgers
    const payload = JSON.stringify(receipt);
    const entryHash = await V.hashEntry(V.hexToBytes(prev), payload);
    bg.entries.push({ payload_json: payload, approvals_json: '[]', prev_hash: prev, entry_hash: V.bytesToHex(entryHash),
      signatures: { ...bg.entries[0].signatures, ed25519_signature: await signEntry('securacv:pwk:break-glass-receipt:v2', entryHash) } });
    bg.count = bg.entries.length;
    bg.head_hash = V.bytesToHex(entryHash);
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
  }

  it('fails an envelope with a second unseal record for one redemption (U2, token_redemption)', async () => {
    const envelope = loadFixture('valid_envelope_unsealed.json');
    const unsealed = envelope.ledgers.break_glass_receipts.entries
      .map((e) => JSON.parse(e.payload_json)).find((r) => r.outcome && r.outcome.Unsealed);
    await appendBreakGlassRow(envelope, unsealed);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    assert.equal(report.failure.kind, 'token_redemption');
    assert.match(report.error, /^break-glass token redemption invalid: redemption [0-9a-f]{64} is recorded as unsealed twice — a token releases one cleartext$/);
  });

  it('fails an envelope with an unseal record naming no redemption (U1, token_redemption)', async () => {
    const envelope = loadFixture('valid_envelope_unsealed.json');
    const unsealed = envelope.ledgers.break_glass_receipts.entries
      .map((e) => JSON.parse(e.payload_json)).find((r) => r.outcome && r.outcome.Unsealed);
    unsealed.outcome.Unsealed.redemption_entry_hash = rep(9);
    await appendBreakGlassRow(envelope, unsealed);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.failure.kind, 'token_redemption');
    assert.equal(report.error, `break-glass token redemption invalid: unseal record names receipt ${'09'.repeat(32)} but no earlier redemption row has that hash`);
  });

  it('fails an envelope whose re-signed export receipt names a grant that does not exist (R2, export_authorization)', async () => {
    const envelope = loadFixture('valid_envelope_unsealed.json');
    const exp = envelope.ledgers.export_receipts;
    const last = exp.entries[exp.entries.length - 1];
    const receipt = JSON.parse(last.payload_json);
    receipt.authorizing_receipt = rep(9);
    const payload = V.serExportReceipt(receipt);
    const entryHash = await V.hashEntry(V.hexToBytes(last.prev_hash), payload);
    const sig = await signEntry('securacv:pwk:export-receipt:v2', entryHash);
    last.payload_json = payload;
    last.entry_hash = V.bytesToHex(entryHash);
    last.signatures = { ...last.signatures, ed25519_signature: sig };
    exp.head_hash = V.bytesToHex(entryHash);
    envelope.export_receipt_entry.receipt = receipt;
    envelope.export_receipt_entry.entry_hash = Array.from(entryHash);
    envelope.export_receipt_entry.signatures = { ...envelope.export_receipt_entry.signatures, ed25519_signature: sig };
    envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);
    const report = await V.verifyEnvelope(envelope);
    assert.equal(report.ok, false);
    assert.equal(report.failure.kind, 'export_authorization');
    assert.equal(report.failure.ledger, 'export_receipts');
    assert.equal(report.error, `export receipt authorization invalid: export receipt names authorizing receipt ${'09'.repeat(32)} but no Granted break-glass receipt has that hash`);
  });
});
