'use strict';

// Evidence-envelope bridge: coarsen the device's raw witness chain into the canonical
// `securacv-evidence-envelope` (v1) so it can be reviewed offline in viewer/evidence_viewer.html
// and verified by either the Rust `envelope_verify` CLI or viewer/verify_core.js.
//
// Why this exists: the raw witness chain (securacv-witness-chain-v1) intentionally carries precise
// data — millisecond ISO timestamps, GPS metadata, thumbnails — because it is the device-side
// tamper-evident ledger. The canonical evidence envelope is the *privacy-coarsened* interchange
// format and its manifest FORBIDS those fields. This bridge enforces that boundary: it buckets time
// to a coarse grid and drops every forbidden field, and it REFUSES to emit an envelope if any
// forbidden datum would survive (coarsen-or-fail) — silently leaking precise data would defeat the
// entire point of the format.
//
// One implementation, both directions: the signed byte layouts (canonical JSON, serde-faithful
// payload serialization, domain-separated signing hash) are imported from the SAME module the
// verifier uses (viewer/verify_core.js), so the device signs exactly the bytes the verifier checks.

const crypto = require('node:crypto');
const path = require('node:path');

const V = require(path.join(__dirname, '..', '..', '..', 'viewer', 'verify_core.js'));
const { computeHash, verifySignature } = require('./witness-chain');

// Signature domains (mirror of src/crypto/signatures.rs). Pulled from the manifest the verifier
// pins, so they cannot drift.
const DOMAINS = V.V1_MANIFEST.signature_domains;

// Default coarse time grid. The manifest permits min 300s; we bucket to 600s (10 min) to match the
// privacy design doc (docs/strategy/07-timeline-events-privacy-design.md).
const DEFAULT_BUCKET_S = V.V1_MANIFEST.time_granularity.default_bucket_s;

// Map the device's raw event_type strings onto the kernel's EventType enum (src/lib.rs).
// Anything not in this table is rejected — we never emit an event the canonical schema can't name.
const EVENT_TYPE_MAP = {
  motion_detected: 'BoundaryCrossingObjectSmall',
  person_detected: 'BoundaryCrossingObjectLarge',
  vehicle_detected: 'VehiclePresenceAfterHours',
  animal_detected: 'BoundaryCrossingObjectSmall',
};

// Raw-record fields that are precise/forbidden and must be DROPPED during coarsening. The builder
// never copies these into the envelope; this list drives the post-build leak assertion below (it is
// expected and normal for raw input records to carry these — coarsening is what removes them).
const DROPPED_RAW_FIELDS = [
  'thumbnail',
  'gps_timestamp',
  'gps_fix_quality',
  'gps_satellites',
  'gps_fix_age_ms',
  'time_source',
];

// Substrings that must never appear anywhere in the serialized canonical envelope. If one does, the
// bridge has a bug and we fail closed rather than emit precise data (coarsen-or-fail, enforced on
// the OUTPUT). `\.\d{3}Z` catches millisecond-precision ISO timestamps that escaped bucketing.
const FORBIDDEN_OUTPUT_MARKERS = ['thumbnail', 'gps_', 'time_source'];
const MILLISECOND_ISO_RE = /\d{2}:\d{2}:\d{2}\.\d{3}Z/;

// A canonical zone identifier is a short, opaque local ID — NOT free text. Coarsening must never
// pass through an operator-typed address, room, or person name. We accept lowercase
// alnum/dash/underscore/colon tokens (e.g. "front", "zone:a", "drive_1"), bounded in length.
// Anything else is refused rather than disclosed.
const ZONE_ID_RE = /^[a-z0-9][a-z0-9_:-]{0,63}$/;

class EnvelopeBridgeError extends Error {
  constructor(message) {
    super(message);
    this.name = 'EnvelopeBridgeError';
  }
}

// Validate the RAW witness chain before coarsening: recompute each record's hash, check it links to
// its predecessor, and verify the device signature. A tampered/reordered/gappy source chain must be
// refused — otherwise re-sealing it would launder a compromised chain into a verifier-OK envelope.
// Mirrors the checks in POST /api/v1/witness/verify (lib/witness-chain.js).
function assertRawChainIntact(records, publicKey) {
  for (let i = 0; i < records.length; i++) {
    const r = records[i];
    const expectedPrev = i > 0 ? records[i - 1].hash : '0'.repeat(64);
    if (r.prev_hash !== expectedPrev) {
      throw new EnvelopeBridgeError(
        `raw chain broken at seq ${r.seq}: prev_hash does not link to the preceding record`);
    }
    const expectedHash = computeHash(
      r.seq, expectedPrev, r.timestamp, r.event_type, r.zone, r.time_source, r.gps_timestamp);
    if (r.hash !== expectedHash) {
      throw new EnvelopeBridgeError(`raw chain broken at seq ${r.seq}: record hash mismatch (tampered record)`);
    }
    if (i > 0 && r.seq !== records[i - 1].seq + 1) {
      throw new EnvelopeBridgeError(
        `raw chain has a sequence gap before seq ${r.seq}; refusing to export a gappy chain without explicit gap records`);
    }
    let sigOk = false;
    try { sigOk = verifySignature(r.hash, r.signature, publicKey); } catch { sigOk = false; }
    if (!sigOk) {
      throw new EnvelopeBridgeError(`raw chain broken at seq ${r.seq}: invalid device signature`);
    }
  }
}

// SHA-256 of the ruleset id (mirror of KernelConfig::ruleset_hash_from_id).
function rulesetHash(rulesetId) {
  return new Uint8Array(crypto.createHash('sha256').update(rulesetId, 'utf8').digest());
}

// The raw 32-byte Ed25519 public key, extracted from the SPKI DER (the key bytes are the trailing
// 32 bytes of the DER structure).
function rawPublicKey(publicKey) {
  const der = publicKey.export({ type: 'spki', format: 'der' });
  return new Uint8Array(der.subarray(der.length - 32));
}

// Sign a 32-byte message digest with Ed25519, returning a 64-element byte array (the shape the
// verifier expects in `signatures.ed25519_signature`).
function signDigest(privateKey, digestBytes) {
  const sig = crypto.sign(null, Buffer.from(digestBytes), privateKey);
  return Array.from(sig);
}

// Coarsen one raw record's millisecond ISO timestamp into a {start_epoch_s, size_s} bucket.
function timeBucketFor(isoTimestamp, bucketS) {
  const ms = Date.parse(isoTimestamp);
  if (!Number.isFinite(ms)) throw new EnvelopeBridgeError(`record has unparseable timestamp: ${isoTimestamp}`);
  const epochS = Math.floor(ms / 1000);
  const start = Math.floor(epochS / bucketS) * bucketS;
  return { start_epoch_s: start, size_s: bucketS };
}

// Defense-in-depth: after building, assert no precise/forbidden datum survived into the serialized
// envelope. The builder is written to only ever read coarse fields, so a hit here is a bug — and we
// fail closed (refuse to emit) rather than leak. This is the OUTPUT side of coarsen-or-fail.
function assertNoLeakedPreciseData(envelope) {
  const serialized = JSON.stringify(envelope);
  for (const marker of FORBIDDEN_OUTPUT_MARKERS) {
    if (serialized.includes(marker)) {
      throw new EnvelopeBridgeError(`coarsening leaked a forbidden field ("${marker}") into the envelope`);
    }
  }
  if (MILLISECOND_ISO_RE.test(serialized)) {
    throw new EnvelopeBridgeError('coarsening leaked a millisecond-precision timestamp into the envelope');
  }
}

// Build one canonical sealed-event payload object from a raw record. Confidence is not present on
// raw records; the canonical schema requires it, so we record a neutral, honest 0.0 (the envelope's
// timeline is about *what/when-coarse/where-zone*, not a re-derived score).
function sealedEventFor(raw, bucketS, provenance) {
  const eventType = EVENT_TYPE_MAP[raw.event_type];
  if (!eventType) {
    throw new EnvelopeBridgeError(`record seq ${raw.seq} has unmappable event_type "${raw.event_type}"`);
  }
  if (typeof raw.zone !== 'string' || raw.zone.length === 0) {
    throw new EnvelopeBridgeError(`record seq ${raw.seq} has no zone`);
  }
  if (!ZONE_ID_RE.test(raw.zone)) {
    throw new EnvelopeBridgeError(
      `record seq ${raw.seq} has a non-local zone id "${raw.zone}"; the canonical envelope only carries ` +
      `opaque local zone identifiers, not free text (which could disclose an address, room, or name)`);
  }
  return {
    event_type: eventType,
    time_bucket: timeBucketFor(raw.timestamp, bucketS),
    zone_id: raw.zone,
    confidence: 0.0,
    correlation_token: null,
    kernel_version: provenance.kernel_version,
    ruleset_id: provenance.ruleset_id,
    ruleset_hash: Array.from(provenance.ruleset_hash),
  };
}

/**
 * Build a canonical securacv-evidence-envelope (v1) from the device's raw witness records.
 *
 * @param {object} opts
 * @param {Array<object>} opts.records       raw witness records (state.witnessRecords)
 * @param {KeyObject} opts.publicKey         device Ed25519 public key
 * @param {KeyObject} opts.privateKey        device Ed25519 private key
 * @param {object} opts.device               { device_id, name, firmware_version }
 * @param {string} [opts.rulesetId]          ruleset identifier (default 'ruleset:canary-vision')
 * @param {number} [opts.bucketS]            coarse time-bucket size in seconds (default 600)
 * @param {number} [opts.nowMs]              clock override for the export receipt bucket (testing)
 * @returns {Promise<object>}                a verifiable evidence envelope
 * @throws {EnvelopeBridgeError}             if any forbidden field would leak, or an event is unmappable
 */
async function buildEvidenceEnvelope(opts) {
  const {
    records, publicKey, privateKey, device,
    rulesetId = 'ruleset:canary-vision',
    bucketS = DEFAULT_BUCKET_S,
    nowMs = Date.now(),
  } = opts;

  if (!Number.isInteger(bucketS) || bucketS < V.V1_MANIFEST.time_granularity.min_bucket_s) {
    throw new EnvelopeBridgeError(`bucketS must be an integer >= ${V.V1_MANIFEST.time_granularity.min_bucket_s}`);
  }

  const provenance = {
    kernel_version: `canary-vision-bridge/${device.firmware_version || '0'}`,
    ruleset_id: rulesetId,
    ruleset_hash: rulesetHash(rulesetId),
  };
  const pubKeyRaw = rawPublicKey(publicKey);

  // Validate the source chain BEFORE coarsening so we never re-seal a tampered/gappy chain.
  assertRawChainIntact(records, publicKey);

  // ---- sealed_events ledger: coarsen each raw record, chain + sign it ----
  const sealedEntries = [];
  let prev = new Uint8Array(32);
  const buckets = new Map(); // start_epoch_s -> { time_bucket, events, failures } (for the artifact)

  for (const raw of records) {
    const ev = sealedEventFor(raw, bucketS, provenance);
    const payloadJson = V.serSealedEvent(ev);
    const entryHash = await V.hashEntry(prev, payloadJson);
    const sigMsg = await V.domainSeparatedHash(DOMAINS.sealed_log_entry, entryHash);
    sealedEntries.push({
      payload_json: payloadJson,
      prev_hash: V.bytesToHex(prev),
      entry_hash: V.bytesToHex(entryHash),
      signatures: { ed25519_scheme: 'ed25519', ed25519_signature: signDigest(privateKey, sigMsg) },
    });
    prev = entryHash;

    const key = ev.time_bucket.start_epoch_s;
    if (!buckets.has(key)) buckets.set(key, { time_bucket: ev.time_bucket, events: [], failures: [] });
    // The artifact event mirrors the sealed event minus the kernel-internal correlation_token.
    buckets.get(key).events.push({
      event_type: ev.event_type,
      time_bucket: ev.time_bucket,
      zone_id: ev.zone_id,
      confidence: ev.confidence,
      kernel_version: ev.kernel_version,
      ruleset_id: ev.ruleset_id,
      ruleset_hash: ev.ruleset_hash,
    });
  }
  const sealedHead = sealedEntries.length ? sealedEntries[sealedEntries.length - 1].entry_hash : null;

  // ---- artifact (human-readable, coarse projection) ----
  const orderedBuckets = [...buckets.values()].sort((a, b) => a.time_bucket.start_epoch_s - b.time_bucket.start_epoch_s);
  const maxEventsPerBatch = 50;
  const artifact = {
    batches: orderedBuckets.length ? [{ buckets: orderedBuckets }] : [],
    max_events_per_batch: maxEventsPerBatch,
    jitter_s: 0,
    jitter_step_s: 60,
  };
  const artifactHash = new Uint8Array(await V.sha256(V.utf8(V.serExportArtifact(artifact))));

  // ---- export_receipts ledger: one signed receipt binding the artifact ----
  const receiptBucket = timeBucketFor(new Date(nowMs).toISOString(), bucketS);
  const receipt = {
    time_bucket: receiptBucket,
    ruleset_hash: Array.from(provenance.ruleset_hash),
    batch_size: maxEventsPerBatch,
    artifact_hash: Array.from(artifactHash),
  };
  const receiptPrev = new Uint8Array(32);
  const receiptEntryHash = await V.hashEntry(receiptPrev, V.serExportReceipt(receipt));
  const receiptSigMsg = await V.domainSeparatedHash(DOMAINS.export_receipt, receiptEntryHash);
  const receiptSig = signDigest(privateKey, receiptSigMsg);
  const exportReceiptEntry = {
    receipt,
    prev_hash: Array.from(receiptPrev),
    entry_hash: Array.from(receiptEntryHash),
    signatures: { ed25519_scheme: 'ed25519', ed25519_signature: receiptSig },
  };

  // ---- assemble the envelope (field order is cosmetic; the verifier canonicalizes) ----
  const envelope = {
    envelope_format: V.ENVELOPE_FORMAT,
    envelope_version: V.ENVELOPE_VERSION,
    manifest: V.V1_MANIFEST,
    provenance: {
      kernel_version: provenance.kernel_version,
      ruleset_id: provenance.ruleset_id,
      ruleset_hash: V.bytesToHex(provenance.ruleset_hash),
      device_public_key: V.bytesToHex(pubKeyRaw),
      pq_public_key: null,
    },
    ledgers: {
      sealed_events: { head_hash: sealedHead, count: sealedEntries.length, entries: sealedEntries },
      break_glass_receipts: { head_hash: null, count: 0, entries: [] },
      export_receipts: {
        head_hash: V.bytesToHex(receiptEntryHash),
        count: 1,
        entries: [{
          payload_json: V.serExportReceipt(receipt),
          prev_hash: V.bytesToHex(receiptPrev),
          entry_hash: V.bytesToHex(receiptEntryHash),
          signatures: { ed25519_scheme: 'ed25519', ed25519_signature: receiptSig },
        }],
      },
      checkpoints: { latest: null },
    },
    artifact,
    gaps: { failure_count: 0, checkpoint_cutoff_event_id: null },
    disclosure: { profile: 'full', disclosed_window: null, break_glass_included: false, redactions: [] },
    export_receipt_entry: exportReceiptEntry,
  };

  envelope.whole_envelope_digest = await V.computeWholeEnvelopeDigest(envelope);

  assertNoLeakedPreciseData(envelope);
  return envelope;
}

module.exports = {
  buildEvidenceEnvelope,
  EnvelopeBridgeError,
  EVENT_TYPE_MAP,
  DROPPED_RAW_FIELDS,
  DEFAULT_BUCKET_S,
  // exported for tests
  _internals: { rulesetHash, rawPublicKey, timeBucketFor },
};
