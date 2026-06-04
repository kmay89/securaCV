// securaCV evidence-envelope verifier core (shared by the offline HTML viewer and
// the Node parity tests). Dependency-free; uses Web Crypto (`globalThis.crypto.subtle`),
// available in modern browsers and Node 20+.
//
// This MUST stay byte-for-byte equivalent to the Rust verifier:
//   - canonical JSON:        src/canonical_json.rs   ("securacv-cjson-v1")
//   - domain separation:     src/crypto/signatures.rs (domain_separated_hash)
//   - envelope verification: src/envelope.rs          (verify_envelope)
// Cross-language parity is pinned by tests/fixtures/envelope/*.
//
// Post-quantum (ML-DSA-44) signatures are NOT verified here: the offline viewer checks
// Ed25519 + the full hash chain and reports PQ as "not checked" (matching SignatureMode::Compat).

'use strict';

const ENVELOPE_FORMAT = 'securacv-evidence-envelope';
const ENVELOPE_VERSION = 1;
const MAX_SAFE = 9007199254740991; // Number.MAX_SAFE_INTEGER

// The canonical v1 manifest (mirror of EvidenceManifest::v1()). A v1 envelope MUST carry this.
const V1_MANIFEST = {
  permitted_fields: ['event_type', 'time_bucket', 'zone_id', 'confidence', 'kernel_version', 'ruleset_id'],
  forbidden_fields: ['raw_media', 'precise_timestamp', 'absolute_location', 'stable_identifier', 'free_text'],
  time_granularity: { min_bucket_s: 300, default_bucket_s: 600 },
  hash_rule: 'SHA256(prev_hash || payload_json_utf8_bytes)',
  canonicalization: 'securacv-cjson-v1',
  signature_domains: {
    sealed_log_entry: 'securacv:pwk:sealed-log-entry:v2',
    checkpoint: 'securacv:pwk:sealed-log-checkpoint:v2',
    break_glass: 'securacv:pwk:break-glass-receipt:v2',
    export_receipt: 'securacv:pwk:export-receipt:v2',
  },
  signature_schemes: { classical: 'ed25519', pq: 'mldsa44' },
};

// ---- byte helpers --------------------------------------------------------

function bytesToHex(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, '0');
  return s;
}

function hexToBytes(hex) {
  if (typeof hex !== 'string' || hex.length % 2 !== 0) throw new Error('invalid hex string');
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

// Accept a 32-byte value encoded either as a hex string (envelope view fields) or as a
// JSON array of byte integers (the embedded ExportReceiptEntry fields).
function toBytes(value) {
  if (typeof value === 'string') return hexToBytes(value);
  if (Array.isArray(value)) return Uint8Array.from(value);
  throw new Error('expected hex string or byte array');
}

function concatBytes(...parts) {
  let len = 0;
  for (const p of parts) len += p.length;
  const out = new Uint8Array(len);
  let off = 0;
  for (const p of parts) { out.set(p, off); off += p.length; }
  return out;
}

function le32(n) {
  return new Uint8Array([n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff]);
}

const utf8 = (s) => new TextEncoder().encode(s);

// ---- canonical JSON ("securacv-cjson-v1") --------------------------------

function canonicalString(s) {
  let out = '"';
  for (const ch of s) {
    const code = ch.codePointAt(0);
    if (ch === '"') out += '\\"';
    else if (ch === '\\') out += '\\\\';
    else if (code === 0x08) out += '\\b';
    else if (code === 0x0c) out += '\\f';
    else if (ch === '\n') out += '\\n';
    else if (ch === '\r') out += '\\r';
    else if (ch === '\t') out += '\\t';
    else if (code < 0x20) out += '\\u' + code.toString(16).padStart(4, '0');
    else out += ch;
  }
  return out + '"';
}

// Compare two strings by UTF-16 code-unit order (matches Rust's encode_utf16 ordering).
function cmpUtf16(a, b) {
  const len = Math.min(a.length, b.length);
  for (let i = 0; i < len; i++) {
    const d = a.charCodeAt(i) - b.charCodeAt(i);
    if (d !== 0) return d;
  }
  return a.length - b.length;
}

function canonicalize(value) {
  if (value === null) return 'null';
  if (value === true) return 'true';
  if (value === false) return 'false';
  if (typeof value === 'number') {
    if (!Number.isInteger(value)) throw new Error('canonical JSON does not permit floating-point numbers');
    if (value < -MAX_SAFE || value > MAX_SAFE) throw new Error('integer outside JS safe-integer range');
    return String(value);
  }
  if (typeof value === 'string') return canonicalString(value);
  if (Array.isArray(value)) return '[' + value.map(canonicalize).join(',') + ']';
  if (typeof value === 'object') {
    const keys = Object.keys(value).sort(cmpUtf16);
    return '{' + keys.map((k) => canonicalString(k) + ':' + canonicalize(value[k])).join(',') + '}';
  }
  throw new Error('unsupported JSON value in canonical serialization');
}

const canonicalBytes = (value) => utf8(canonicalize(value));

// ---- crypto --------------------------------------------------------------

async function sha256(bytes) {
  const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes);
  return new Uint8Array(digest);
}

async function domainSeparatedHash(domain, entryHash) {
  const d = utf8(domain);
  return sha256(concatBytes(le32(d.length), d, entryHash));
}

async function hashEntry(prevHash, payloadStr) {
  return sha256(concatBytes(prevHash, utf8(payloadStr)));
}

async function verifyEd25519(pubKeyBytes, signatureBytes, message) {
  let key;
  try {
    key = await globalThis.crypto.subtle.importKey('raw', pubKeyBytes, { name: 'Ed25519' }, false, ['verify']);
  } catch (e) {
    throw new Error('this runtime lacks Ed25519 Web Crypto support: ' + e.message);
  }
  return globalThis.crypto.subtle.verify('Ed25519', key, signatureBytes, message);
}

// Probe whether this runtime can verify Ed25519 at all. Older engines (Safari < 17,
// pre-2023 Firefox) ship Web Crypto but not the Ed25519 algorithm; importKey throws there.
// We use this to report "inconclusive" instead of falsely rejecting valid evidence.
async function ed25519Supported() {
  try {
    // RFC 8032 §7.1 test-vector public key — a known-valid 32-byte Ed25519 key.
    const probe = hexToBytes('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a');
    await globalThis.crypto.subtle.importKey('raw', probe, { name: 'Ed25519' }, false, ['verify']);
    return true;
  } catch {
    return false;
  }
}

// ---- serde-faithful serialization (for hashing the artifact and receipt) -------------
// These reproduce `serde_json::to_vec(...)` byte-for-byte: struct field order is fixed (NOT the
// input JSON's key order), integers print bare, and the single float field (`confidence`, an f32)
// is formatted to match serde/ryu. Empirically, re-stringifying serde's f32 text in JS matches
// for every value except integer-valued floats, where serde writes "1.0"/"0.0" — handled below.

function serInt(n) {
  if (typeof n !== 'number' || !Number.isInteger(n)) throw new Error('expected integer, got ' + n);
  return String(n);
}

function serF32(v) {
  if (typeof v !== 'number' || !isFinite(v)) throw new Error('invalid f32 value: ' + v);
  if (Number.isInteger(v)) return v.toFixed(1); // serde prints 1.0f32 as "1.0", 0.0f32 as "0.0"
  const s = String(v);
  if (s.indexOf('e') !== -1 || s.indexOf('E') !== -1) {
    throw new Error('f32 value uses exponent notation; cannot reproduce serde bytes: ' + s);
  }
  return s;
}

const serBytes = (arr) => '[' + Array.from(arr, serInt).join(',') + ']';
const serStr = (s) => canonicalString(s); // serde-compatible escaping
function serEnum(v) {
  if (typeof v !== 'string') throw new Error('expected unit enum string');
  return serStr(v);
}
const serTimeBucket = (tb) => `{"start_epoch_s":${serInt(tb.start_epoch_s)},"size_s":${serInt(tb.size_s)}}`;

function serExportEvent(e) {
  return `{"event_type":${serEnum(e.event_type)},"time_bucket":${serTimeBucket(e.time_bucket)},` +
    `"zone_id":${serStr(e.zone_id)},"confidence":${serF32(e.confidence)},` +
    `"kernel_version":${serStr(e.kernel_version)},"ruleset_id":${serStr(e.ruleset_id)},` +
    `"ruleset_hash":${serBytes(e.ruleset_hash)}}`;
}

function serExportFailure(f) {
  return `{"failure_type":${serEnum(f.failure_type)},"time_bucket":${serTimeBucket(f.time_bucket)},` +
    `"details":${f.details == null ? 'null' : serStr(f.details)},` +
    `"kernel_version":${serStr(f.kernel_version)},"ruleset_id":${serStr(f.ruleset_id)},` +
    `"ruleset_hash":${serBytes(f.ruleset_hash)}}`;
}

function serExportBucket(b) {
  return `{"time_bucket":${serTimeBucket(b.time_bucket)},` +
    `"events":[${b.events.map(serExportEvent).join(',')}],` +
    `"failures":[${b.failures.map(serExportFailure).join(',')}]}`;
}

const serExportBatch = (bt) => `{"buckets":[${bt.buckets.map(serExportBucket).join(',')}]}`;

function serExportArtifact(a) {
  return `{"batches":[${a.batches.map(serExportBatch).join(',')}],` +
    `"max_events_per_batch":${serInt(a.max_events_per_batch)},` +
    `"jitter_s":${serInt(a.jitter_s)},"jitter_step_s":${serInt(a.jitter_step_s)}}`;
}

function serExportReceipt(r) {
  return `{"time_bucket":${serTimeBucket(r.time_bucket)},"ruleset_hash":${serBytes(r.ruleset_hash)},` +
    `"batch_size":${serInt(r.batch_size)},"artifact_hash":${serBytes(r.artifact_hash)}}`;
}

// Sealed-log records are stored as the literal `payload_json` string and hashed verbatim by both
// verifiers, so the builder must serialize them in serde tagged-enum field order:
// `{"record_type":...,<variant fields>}`. (The Event variant carries `correlation_token`, which the
// canary bridge always leaves null — the firmware-side correlation tokens are not exported.)
function serSealedEvent(e) {
  return `{"record_type":"event","event_type":${serEnum(e.event_type)},` +
    `"time_bucket":${serTimeBucket(e.time_bucket)},"zone_id":${serStr(e.zone_id)},` +
    `"confidence":${serF32(e.confidence)},` +
    `"correlation_token":${e.correlation_token == null ? 'null' : serBytes(e.correlation_token)},` +
    `"kernel_version":${serStr(e.kernel_version)},"ruleset_id":${serStr(e.ruleset_id)},` +
    `"ruleset_hash":${serBytes(e.ruleset_hash)}}`;
}

function serSealedFailure(f) {
  return `{"record_type":"failure","failure_type":${serEnum(f.failure_type)},` +
    `"time_bucket":${serTimeBucket(f.time_bucket)},` +
    `"details":${f.details == null ? 'null' : serStr(f.details)},` +
    `"kernel_version":${serStr(f.kernel_version)},"ruleset_id":${serStr(f.ruleset_id)},` +
    `"ruleset_hash":${serBytes(f.ruleset_hash)}}`;
}

// ---- envelope digest -----------------------------------------------------

async function computeWholeEnvelopeDigest(envelope) {
  const input = JSON.parse(JSON.stringify(envelope));
  delete input.whole_envelope_digest;
  // Bind the artifact by reference (its committed hash), keeping the digest input float-free.
  input.artifact = bytesToHex(toBytes(envelope.export_receipt_entry.receipt.artifact_hash));
  return bytesToHex(await sha256(canonicalBytes(input)));
}

// ---- break-glass approvals commitment ------------------------------------

function canonicalApprovalBytes(approval) {
  const trustee = utf8(typeof approval.trustee === 'string' ? approval.trustee : approval.trustee[0]);
  const reqHash = toBytes(approval.request_hash);
  const sig = Uint8Array.from(approval.signature);
  return concatBytes(le32(trustee.length), trustee, reqHash, le32(sig.length), sig);
}

async function approvalsCommitment(approvals) {
  const entries = approvals.map(canonicalApprovalBytes);
  entries.sort((a, b) => {
    const len = Math.min(a.length, b.length);
    for (let i = 0; i < len; i++) { if (a[i] !== b[i]) return a[i] - b[i]; }
    return a.length - b.length;
  });
  const parts = [];
  for (const e of entries) parts.push(le32(e.length), e);
  return sha256(concatBytes(...parts));
}

// ---- chain walk ----------------------------------------------------------

function bytesEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

async function verifyLinearChain(entries, startHead, domain, pubKey, ledgerName) {
  let expectedPrev = startHead;
  for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    const prev = toBytes(e.prev_hash);
    const entryHash = toBytes(e.entry_hash);
    if (!bytesEqual(prev, expectedPrev)) {
      throw new Error(`${ledgerName} integrity check failed at index ${i}: prev_hash mismatch`);
    }
    const computed = await hashEntry(expectedPrev, e.payload_json);
    if (!bytesEqual(computed, entryHash)) {
      throw new Error(`${ledgerName} integrity check failed at index ${i}: entry_hash mismatch`);
    }
    const sig = Uint8Array.from(e.signatures.ed25519_signature);
    const msg = await domainSeparatedHash(domain, entryHash);
    if (!(await verifyEd25519(pubKey, sig, msg))) {
      throw new Error(`${ledgerName} signature verification failed at index ${i}`);
    }
    expectedPrev = entryHash;
  }
  return expectedPrev;
}

function validateSummary(name, entriesLen, declaredCount, declaredHeadRaw, computedHead) {
  if (declaredCount !== entriesLen) {
    throw new Error(`${name} count mismatch: declared=${declaredCount}, actual=${entriesLen}`);
  }
  const declaredHead = declaredHeadRaw == null ? null : toBytes(declaredHeadRaw);
  if (entriesLen === 0) {
    if (declaredHead !== null) throw new Error(`${name} head_hash should be null for an empty ledger`);
  } else if (declaredHead === null || !bytesEqual(declaredHead, computedHead)) {
    throw new Error(`${name} head_hash does not match the verified chain head`);
  }
}

// ---- top-level verify ----------------------------------------------------

// Returns { status, ok, error?, sealed_events, break_glass_granted, break_glass_denied,
//           export_receipts, pq_checked, warnings, whole_envelope_digest }.
async function verifyEnvelope(envelope) {
  const result = {
    status: 'compromised',
    ok: false,
    sealed_events: 0,
    break_glass_granted: 0,
    break_glass_denied: 0,
    export_receipts: 0,
    pq_checked: false,
    warnings: [],
    checks: [],
    whole_envelope_digest: envelope && envelope.whole_envelope_digest,
  };
  try {
    if (envelope.envelope_format !== ENVELOPE_FORMAT) throw new Error(`unknown envelope_format: ${envelope.envelope_format}`);
    if (envelope.envelope_version !== ENVELOPE_VERSION) throw new Error(`unsupported envelope_version ${envelope.envelope_version}`);
    result.checks.push('Envelope format and version recognized');

    // Manifest must equal the canonical v1 manifest ("rules travel with the data").
    if (canonicalize(envelope.manifest) !== canonicalize(V1_MANIFEST)) {
      throw new Error('manifest does not match the canonical v1 manifest for envelope_version 1');
    }
    result.checks.push('Manifest matches the canonical v1 ruleset');

    const expectedDigest = await computeWholeEnvelopeDigest(envelope);
    if (expectedDigest !== envelope.whole_envelope_digest) {
      throw new Error(`whole_envelope_digest mismatch: computed=${expectedDigest}, stored=${envelope.whole_envelope_digest}`);
    }
    result.checks.push('Bundle fingerprint recomputed and matches');

    // The remaining checks recompute and verify Ed25519 signatures. If this runtime can't do
    // Ed25519 (e.g. Safari < 17, older Firefox), we must NOT call the bundle "compromised" —
    // that would be a false accusation against potentially valid evidence. Stop here with an
    // honest "inconclusive" verdict so the user knows the bundle was neither passed nor failed.
    if (!(await ed25519Supported())) {
      result.status = 'inconclusive';
      result.ok = false;
      result.inconclusive = true;
      result.warnings.push('signatures not checked: this browser lacks Ed25519 Web Crypto support');
      result.error = 'This browser cannot check Ed25519 signatures, so signature verification could not run. ' +
        'The bundle was NOT rejected — its structure and fingerprint checked out. To complete the check, ' +
        'reopen this page in a current Chrome, Edge, Firefox, or Safari 17+, or run the securaCV Rust verifier.';
      return result;
    }

    const pubKey = hexToBytes(envelope.provenance.device_public_key);
    const domains = envelope.manifest.signature_domains;

    // Sealed events (start at checkpoint head if present, else genesis).
    const cp = envelope.ledgers.checkpoints.latest;
    const sealedStart = cp ? toBytes(cp.chain_head_hash) : new Uint8Array(32);
    const sealed = envelope.ledgers.sealed_events;
    const sealedHead = await verifyLinearChain(sealed.entries, sealedStart, domains.sealed_log_entry, pubKey, 'sealed_events');
    validateSummary('sealed_events', sealed.entries.length, sealed.count, sealed.head_hash, sealedHead);
    result.checks.push(`Sealed-event hash chain intact (${sealed.entries.length} ${sealed.entries.length === 1 ? 'entry' : 'entries'})`);
    result.checks.push('Device Ed25519 signatures valid on every sealed event');

    // Checkpoint signature.
    if (cp) {
      const head = toBytes(cp.chain_head_hash);
      const msg = await domainSeparatedHash(domains.checkpoint, head);
      const sig = Uint8Array.from(cp.signatures.ed25519_signature);
      if (!(await verifyEd25519(pubKey, sig, msg))) throw new Error('checkpoint signature verification failed');
      result.checks.push('Checkpoint signature valid (chain continuity across pruning)');
    }

    // Break-glass receipts.
    const bg = envelope.ledgers.break_glass_receipts;
    const bgHead = await verifyLinearChain(bg.entries, new Uint8Array(32), domains.break_glass, pubKey, 'break_glass_receipts');
    validateSummary('break_glass_receipts', bg.entries.length, bg.count, bg.head_hash, bgHead);
    for (const e of bg.entries) {
      const receipt = JSON.parse(e.payload_json);
      const approvals = JSON.parse(e.approvals_json);
      const commitment = bytesToHex(await approvalsCommitment(approvals));
      if (commitment !== bytesToHex(toBytes(receipt.approvals_commitment))) {
        throw new Error('break-glass approvals commitment mismatch');
      }
      if (receipt.outcome === 'Granted' || (receipt.outcome && receipt.outcome.Granted !== undefined)) result.break_glass_granted++;
      else result.break_glass_denied++;
    }
    if (bg.entries.length > 0) result.checks.push(`Break-glass receipts verified (${bg.entries.length})`);

    // Export receipts.
    const exp = envelope.ledgers.export_receipts;
    const expHead = await verifyLinearChain(exp.entries, new Uint8Array(32), domains.export_receipt, pubKey, 'export_receipts');
    validateSummary('export_receipts', exp.entries.length, exp.count, exp.head_hash, expHead);

    // Provenance must be consistent with the signed export receipt.
    if (bytesToHex(toBytes(envelope.provenance.ruleset_hash)) !== bytesToHex(toBytes(envelope.export_receipt_entry.receipt.ruleset_hash))) {
      throw new Error('provenance ruleset_hash does not match the signed export receipt ruleset_hash');
    }

    // Bind the human-readable artifact to the signed receipt: recompute its hash exactly as the
    // device did (serde-faithful bytes) and require it to equal the signed artifact_hash. Without
    // this, a tampered `artifact` would pass — the offline viewer must not bless doctored contents.
    const artifactHash = await sha256(utf8(serExportArtifact(envelope.artifact)));
    if (!bytesEqual(artifactHash, toBytes(envelope.export_receipt_entry.receipt.artifact_hash))) {
      throw new Error('export artifact hash mismatch');
    }
    result.checks.push('Human-readable artifact bound to the signed export hash');

    // Export-receipt entry: serialize the receipt in serde field order (NOT the input JSON's key
    // order) so a key-reordered-but-equivalent bundle still re-derives the signed entry_hash.
    const rEntry = envelope.export_receipt_entry;
    const computedEntryHash = await hashEntry(toBytes(rEntry.prev_hash), serExportReceipt(rEntry.receipt));
    if (!bytesEqual(computedEntryHash, toBytes(rEntry.entry_hash))) {
      throw new Error('export receipt entry_hash mismatch');
    }
    const rMsg = await domainSeparatedHash(domains.export_receipt, toBytes(rEntry.entry_hash));
    const rSig = Uint8Array.from(rEntry.signatures.ed25519_signature);
    if (!(await verifyEd25519(pubKey, rSig, rMsg))) throw new Error('export receipt signature verification failed');
    result.checks.push('Export receipt signature valid (provenance bound)');

    // Warnings & status.
    if (envelope.provenance.pq_public_key) result.warnings.push('post-quantum signatures present but not verified in this offline viewer');
    else result.warnings.push('no post-quantum public key; PQ signatures not checked');
    if (envelope.gaps && envelope.gaps.failure_count > 0) result.warnings.push(`${envelope.gaps.failure_count} failure record(s) present (explicit gaps)`);
    if (envelope.disclosure && envelope.disclosure.redactions && envelope.disclosure.redactions.length > 0) result.warnings.push('disclosure contains redactions');

    result.sealed_events = sealed.entries.length;
    result.export_receipts = exp.entries.length;
    result.ok = true;
    result.status = result.warnings.length === 0 ? 'ok' : 'valid_with_warnings';
    return result;
  } catch (err) {
    result.ok = false;
    result.status = 'compromised';
    result.error = err.message || String(err);
    return result;
  }
}

const api = {
  verifyEnvelope,
  ed25519Supported,
  computeWholeEnvelopeDigest,
  canonicalize,
  canonicalBytes,
  domainSeparatedHash,
  approvalsCommitment,
  bytesToHex,
  hexToBytes,
  V1_MANIFEST,
  ENVELOPE_FORMAT,
  ENVELOPE_VERSION,
  // Low-level primitives reused by the envelope *builder* (canary-vision bridge) so that
  // construction and verification share one implementation — the bytes a device signs are
  // produced by the exact code that later checks them. See canary-vision/device-api/lib/envelope-bridge.js.
  sha256,
  hashEntry,
  serExportEvent,
  serExportFailure,
  serExportArtifact,
  serExportReceipt,
  serSealedEvent,
  serSealedFailure,
  utf8,
};

if (typeof module !== 'undefined' && module.exports) module.exports = api;
if (typeof globalThis !== 'undefined') globalThis.SecuraCVVerify = api;
