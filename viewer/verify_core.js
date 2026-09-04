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

// ---- device-key rotation lineage -----------------------------------------
// Mirrors src/crypto/signatures.rs (rotation_binding_hash) and
// src/envelope.rs (envelope_key_lineage). A rotated device's envelope carries
// rows signed by earlier keys; the evidence for trusting those keys travels
// IN the envelope as KeyRotation records (new key's possession attestation +
// prev key's authorization, distinct domains), walked BACKWARD from the
// provenance anchor.

const DOMAIN_KEY_ROTATION = 'securacv:pwk:device-key-rotation:v1';
const DOMAIN_KEY_ROTATION_AUTHZ = 'securacv:pwk:device-key-rotation-authz:v1';

async function rotationBindingHash(domain, prevKeyBytes, newKeyBytes) {
  const inner = await sha256(concatBytes(prevKeyBytes, newKeyBytes));
  return domainSeparatedHash(domain, inner);
}

// Returns { keys: [Uint8Array...oldest->newest], rowKey: [keyIndex per sealed row] }.
// Throws on a CHAINING rotation whose attestation/authorization fails (tampered
// identity evidence). Rotations that do not chain into the anchor cannot extend
// the lineage; their rows still verify (or fail) like any other row.
async function envelopeKeyLineage(entries, provenanceKeyBytes) {
  const rotations = [];
  for (let row = 0; row < entries.length; row++) {
    let parsed;
    try { parsed = JSON.parse(entries[row].payload_json); } catch { continue; }
    if (!parsed || parsed.record_type !== 'key_rotation') continue;
    rotations.push({
      row,
      prev: Uint8Array.from(parsed.prev_public_key || []),
      next: Uint8Array.from(parsed.new_public_key || []),
      attestation: Uint8Array.from(parsed.new_key_attestation || []),
      authorization: Uint8Array.from(parsed.prev_key_authorization || []),
    });
  }
  const keysRev = [provenanceKeyBytes];
  const chainedRows = [];
  let current = provenanceKeyBytes;
  for (let i = rotations.length - 1; i >= 0; i--) {
    const rot = rotations[i];
    if (rot.next.length !== 32 || rot.prev.length !== 32 || !bytesEqual(rot.next, current)) continue;
    const attMsg = await rotationBindingHash(DOMAIN_KEY_ROTATION, rot.prev, rot.next);
    if (!(await verifyEd25519(rot.next, rot.attestation, attMsg))) {
      throw chainFail(`key rotation at row ${rot.row}: possession attestation invalid`,
        'sealed_events', rot.row, 'key_rotation_invalid');
    }
    if (rot.authorization.length > 0) {
      const authzMsg = await rotationBindingHash(DOMAIN_KEY_ROTATION_AUTHZ, rot.prev, rot.next);
      if (!(await verifyEd25519(rot.prev, rot.authorization, authzMsg))) {
        throw chainFail(`key rotation at row ${rot.row}: predecessor authorization invalid`,
          'sealed_events', rot.row, 'key_rotation_invalid');
      }
    }
    // Legacy records (empty authorization) are anchored by their row's entry
    // signature under the predecessor, which the chain walk enforces because
    // the row's assigned signer IS that predecessor.
    keysRev.push(rot.prev);
    chainedRows.push(rot.row);
    current = rot.prev;
  }
  keysRev.reverse();
  chainedRows.reverse();
  // Rows up to and INCLUDING a chaining rotation row are signed by the
  // retiring key; the successor signs from the next row on.
  const rowKey = [];
  let active = 0;
  let nextRot = 0;
  for (let row = 0; row < entries.length; row++) {
    rowKey.push(active);
    if (nextRot < chainedRows.length && chainedRows[nextRot] === row) { active++; nextRot++; }
  }
  return { keys: keysRev, rowKey };
}

async function verifyEd25519AnyOf(keys, signatureBytes, message) {
  for (const key of keys) {
    if (await verifyEd25519(key, signatureBytes, message)) return true;
  }
  return false;
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
// Cached: support is static for the page's lifetime, and caching the promise also collapses
// concurrent verifications onto a single probe.
let ed25519SupportPromise = null;
function ed25519Supported() {
  if (!ed25519SupportPromise) {
    ed25519SupportPromise = (async () => {
      try {
        // RFC 8032 §7.1 test-vector public key — a known-valid 32-byte Ed25519 key.
        const probe = hexToBytes('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a');
        await globalThis.crypto.subtle.importKey('raw', probe, { name: 'Ed25519' }, false, ['verify']);
        return true;
      } catch {
        return false;
      }
    })();
  }
  return ed25519SupportPromise;
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

const serExportWindow = (w) =>
  `{"start_epoch_s":${serInt(w.start_epoch_s)},"end_epoch_s":${serInt(w.end_epoch_s)}}`;

// `auth_mode` and `window` are optional trailing fields the Rust side skips when
// absent (legacy receipts); serialize them only when present so both receipt
// generations hash to the bytes the device signed.
function serExportReceipt(r) {
  let s = `{"time_bucket":${serTimeBucket(r.time_bucket)},"ruleset_hash":${serBytes(r.ruleset_hash)},` +
    `"batch_size":${serInt(r.batch_size)},"artifact_hash":${serBytes(r.artifact_hash)}`;
  if (r.auth_mode != null) s += `,"auth_mode":${serEnum(r.auth_mode)}`;
  if (r.window != null) s += `,"window":${serExportWindow(r.window)}`;
  return s + '}';
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

// Throw with a structured `failure` attached: {ledger, entry_id, kind} mirrors the Rust
// verifier's VerifyFailure (same serde wire values), pinned cross-language by the
// tampered_payload fixture in both test suites. The message text is unchanged.
function chainFail(message, ledger, entryId, kind) {
  const err = new Error(message);
  err.failure = { ledger, entry_id: entryId, kind };
  return err;
}

// `signers` mirrors src/envelope.rs ChainSigners: rows are signed by the
// device key current at write time, so a rotated device's ledgers mix signers.
//   { perEntry: keys, rowKey }  sealed events — row i must verify under
//                               keys[rowKey[i]] (switches at each rotation)
//   { anyOf: keys }             receipts/checkpoint — any validated lineage key
async function verifyLinearChain(entries, startHead, domain, signers, ledgerName, verifySig) {
  let expectedPrev = startHead;
  for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    const prev = toBytes(e.prev_hash);
    const entryHash = toBytes(e.entry_hash);
    if (!bytesEqual(prev, expectedPrev)) {
      throw chainFail(`${ledgerName} integrity check failed at index ${i}: prev_hash mismatch`,
        ledgerName, i, 'prev_hash_mismatch');
    }
    const computed = await hashEntry(expectedPrev, e.payload_json);
    if (!bytesEqual(computed, entryHash)) {
      throw chainFail(`${ledgerName} integrity check failed at index ${i}: entry_hash mismatch`,
        ledgerName, i, 'entry_hash_mismatch');
    }
    // The hash chain above is proven with SHA-256 alone. Only the Ed25519 signature needs Web
    // Crypto's Ed25519 — defer just that verdict when the runtime can't do it (see verifyEnvelope).
    if (verifySig) {
      const sig = Uint8Array.from(e.signatures.ed25519_signature);
      const msg = await domainSeparatedHash(domain, entryHash);
      const ok = signers.perEntry
        ? await verifyEd25519(signers.perEntry[signers.rowKey[i]], sig, msg)
        : await verifyEd25519AnyOf(signers.anyOf, sig, msg);
      if (!ok) {
        throw chainFail(`${ledgerName} signature verification failed at index ${i}`,
          ledgerName, i, 'signature_mismatch');
      }
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

    // Signatures are the ONLY checks that need Ed25519 Web Crypto. Older engines (Safari < 17,
    // pre-2023 Firefox) lack it. Rather than bail out here — which would skip deterministic checks
    // like the artifact-hash binding and make valid evidence look "rejected" — we run EVERY
    // hash/structure check below and defer only the signature verdict, then return an honest
    // "inconclusive" status at the end. Tampering is still caught definitively either way.
    const sigCheckable = await ed25519Supported();

    const pubKey = sigCheckable ? hexToBytes(envelope.provenance.device_public_key) : null;
    const domains = envelope.manifest.signature_domains;

    // The provenance key is the device key CURRENT at seal time; a rotated
    // device's ledgers carry rows signed by earlier lineage keys, proven by
    // the envelope's own KeyRotation records. Lineage validation needs
    // Ed25519, so it rides the same sigCheckable deferral as every signature.
    const lineage = sigCheckable
      ? await envelopeKeyLineage(envelope.ledgers.sealed_events.entries, pubKey)
      : { keys: [], rowKey: [] };
    if (sigCheckable && lineage.keys.length > 1) {
      result.checks.push(`Device key rotation(s) followed (${lineage.keys.length - 1}) — lineage anchored at the provenance key`);
    }

    // Sealed events (start at checkpoint head if present, else genesis).
    const cp = envelope.ledgers.checkpoints.latest;
    const sealedStart = cp ? toBytes(cp.chain_head_hash) : new Uint8Array(32);
    const sealed = envelope.ledgers.sealed_events;
    const sealedHead = await verifyLinearChain(sealed.entries, sealedStart, domains.sealed_log_entry,
      { perEntry: lineage.keys, rowKey: lineage.rowKey }, 'sealed_events', sigCheckable);
    validateSummary('sealed_events', sealed.entries.length, sealed.count, sealed.head_hash, sealedHead);
    result.checks.push(`Sealed-event hash chain intact (${sealed.entries.length} ${sealed.entries.length === 1 ? 'entry' : 'entries'})`);
    if (sigCheckable) result.checks.push('Device Ed25519 signatures valid on every sealed event');

    // Checkpoint signature — by whichever lineage key was current when it was written.
    if (cp && sigCheckable) {
      const head = toBytes(cp.chain_head_hash);
      const msg = await domainSeparatedHash(domains.checkpoint, head);
      const sig = Uint8Array.from(cp.signatures.ed25519_signature);
      if (!(await verifyEd25519AnyOf(lineage.keys, sig, msg))) {
        throw chainFail('checkpoint signature verification failed', 'checkpoint', null, 'checkpoint_invalid');
      }
      result.checks.push('Checkpoint signature valid (chain continuity across pruning)');
    }

    // Break-glass receipts.
    const bg = envelope.ledgers.break_glass_receipts;
    const bgHead = await verifyLinearChain(bg.entries, new Uint8Array(32), domains.break_glass,
      { anyOf: lineage.keys }, 'break_glass_receipts', sigCheckable);
    validateSummary('break_glass_receipts', bg.entries.length, bg.count, bg.head_hash, bgHead);
    for (let i = 0; i < bg.entries.length; i++) {
      const e = bg.entries[i];
      const receipt = JSON.parse(e.payload_json);
      const approvals = JSON.parse(e.approvals_json);
      const commitment = bytesToHex(await approvalsCommitment(approvals));
      if (commitment !== bytesToHex(toBytes(receipt.approvals_commitment))) {
        throw chainFail('break-glass approvals commitment mismatch',
          'break_glass_receipts', i, 'approvals_commitment_mismatch');
      }
      if (receipt.outcome === 'Granted' || (receipt.outcome && receipt.outcome.Granted !== undefined)) result.break_glass_granted++;
      else result.break_glass_denied++;
    }
    if (bg.entries.length > 0) result.checks.push(`Break-glass receipts verified (${bg.entries.length})`);

    // Export receipts.
    const exp = envelope.ledgers.export_receipts;
    const expHead = await verifyLinearChain(exp.entries, new Uint8Array(32), domains.export_receipt,
      { anyOf: lineage.keys }, 'export_receipts', sigCheckable);
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
    if (sigCheckable) {
      // Strictly the provenance key, never any-of: this entry is created in
      // the SAME export call that stamps provenance, so accepting an earlier
      // lineage key here would let the holder of a RETIRED (possibly
      // compromised) key re-sign a forged artifact+receipt and recompute the
      // unsigned whole-envelope digest. Only the ledger walks accept
      // historical signers.
      const rMsg = await domainSeparatedHash(domains.export_receipt, toBytes(rEntry.entry_hash));
      const rSig = Uint8Array.from(rEntry.signatures.ed25519_signature);
      if (!(await verifyEd25519(pubKey, rSig, rMsg))) {
        throw new Error('export receipt signature does not verify under the provenance key (the key current at seal time)');
      }
      result.checks.push('Export receipt signature valid (provenance bound)');
    }
    // And that entry must BE the head of the presented export-receipts ledger:
    // the export that sealed this envelope appended its receipt last, so a
    // ledger whose verified head is a different row is a substituted ledger.
    // With the head signature pinned to the current key, chain linkage then
    // protects every interior row against a retired-key holder.
    if (!bytesEqual(expHead, toBytes(rEntry.entry_hash))) {
      throw new Error('export-receipts ledger head does not match the envelope\'s export receipt entry — the receipt ledger is not the one this export sealed');
    }

    // Warnings & status.
    if (envelope.provenance.pq_public_key) result.warnings.push('post-quantum signatures present but not verified in this offline viewer');
    else result.warnings.push('no post-quantum public key; PQ signatures not checked');
    // failure_count is derived from the artifact at assembly (artifact_hash is signature-bound;
    // gaps is not), so re-derive it rather than trust the declared value — matches src/envelope.rs.
    const derivedFailures = ((envelope.artifact && envelope.artifact.batches) || [])
      .reduce((n, b) => n + ((b.buckets || []).reduce((m, k) => m + ((k.failures || []).length), 0)), 0);
    const declaredFailures = envelope.gaps ? envelope.gaps.failure_count : undefined;
    if (declaredFailures !== derivedFailures) {
      throw new Error(`gaps.failure_count ${declaredFailures} does not match the ${derivedFailures} failure record(s) in the signed artifact`);
    }
    if (derivedFailures > 0) result.warnings.push(`${derivedFailures} failure record(s) present (explicit gaps)`);
    if (envelope.disclosure && envelope.disclosure.redactions && envelope.disclosure.redactions.length > 0) result.warnings.push('disclosure contains redactions');

    result.sealed_events = sealed.entries.length;
    result.export_receipts = exp.entries.length;

    // Every deterministic (hash / structure / artifact-binding) check above passed, so any tampering
    // would already have thrown. If the runtime couldn't run the Ed25519 checks, the verdict is
    // "inconclusive" — not a pass: contents are internally consistent and untampered, but device
    // authorship was not cryptographically confirmed here.
    if (!sigCheckable) {
      result.status = 'inconclusive';
      result.ok = false;
      result.inconclusive = true;
      result.warnings.unshift('signatures not checked: this browser lacks Ed25519 Web Crypto support');
      result.error = 'This browser cannot check Ed25519 signatures, so device authorship was not confirmed. ' +
        'The bundle was NOT rejected — its structure, hash chains, fingerprint, and the artifact-to-signed-hash ' +
        'binding all checked out, so any tampering would have been caught. To confirm the signatures too, reopen ' +
        'this page in a current Chrome, Edge, Firefox, or Safari 17+, or run the securaCV Rust verifier.';
      return result;
    }

    result.ok = true;
    result.status = result.warnings.length === 0 ? 'ok' : 'valid_with_warnings';
    return result;
  } catch (err) {
    result.ok = false;
    result.status = 'compromised';
    result.error = err.message || String(err);
    result.failure = err.failure || null;
    return result;
  }
}

// ---- export-bundle verification ------------------------------------------
// Mirrors the Rust `verify_export_bundle` (src/lib.rs) check-for-check and in
// the same order: receipt entry hash → signature → artifact binding. Pinned to
// the same fixture as the Rust side (tests/fixtures/export_bundle/).
//
// Trust caveat, mirrored from `export_verify`: the bundle carries its own
// device key, so without a caller-supplied trusted key the verdict means
// "internally consistent and signed by the key the bundle names" — authorship
// is self-attested, not confirmed against an external identity. Passing
// `opts.trustedDeviceKeyHex` upgrades the verdict to confirmed authorship
// (or fails it, if the keys differ).

function looksLikeExportBundle(obj) {
  return !!(obj && typeof obj === 'object' && obj.artifact && obj.receipt_entry && obj.device_public_key);
}

function looksLikeEnvelope(obj) {
  return !!(obj && typeof obj === 'object' && obj.envelope_format === ENVELOPE_FORMAT);
}

async function verifyExportBundle(bundle, opts = {}) {
  const result = {
    kind: 'export_bundle',
    ok: false, status: 'compromised', checks: [], warnings: [], error: null,
    events: 0, failures: 0, buckets: 0, batches: 0,
    authorship: 'bundle', device_public_key: null, artifact_hash: null, entry_hash: null,
  };
  try {
    if (!looksLikeExportBundle(bundle)) {
      throw new Error('not an export bundle: missing artifact / receipt_entry / device_public_key');
    }
    const pubKey = toBytes(bundle.device_public_key);
    if (pubKey.length !== 32) throw new Error('device_public_key must be 32 bytes');
    result.device_public_key = bytesToHex(pubKey);
    result.checks.push('Export bundle structure recognized');

    // Authorship: compare the bundle's key against a caller-supplied trusted
    // key BEFORE the signature check, so "verified" means verified under the
    // key the caller trusts — the same semantics as the Rust `export_verify`.
    const trustedHex = (opts.trustedDeviceKeyHex || '').trim().toLowerCase();
    if (trustedHex) {
      let trusted;
      try { trusted = hexToBytes(trustedHex); } catch { throw new Error('the supplied device key is not valid hex'); }
      if (trusted.length !== 32) throw new Error('the supplied device key must be 32 bytes (64 hex characters)');
      if (!bytesEqual(trusted, pubKey)) {
        throw new Error('device key mismatch: this bundle names a different device key than the one you supplied');
      }
      result.authorship = 'trusted';
      result.checks.push('Bundle device key matches the trusted key you supplied');
    }

    // 1. Entry hash: serialize the receipt in serde field order (NOT the input
    //    JSON's key order) so a key-reordered-but-equivalent bundle still
    //    re-derives the signed entry_hash.
    const rEntry = bundle.receipt_entry;
    const computedEntryHash = await hashEntry(toBytes(rEntry.prev_hash), serExportReceipt(rEntry.receipt));
    if (!bytesEqual(computedEntryHash, toBytes(rEntry.entry_hash))) {
      throw new Error('export receipt entry_hash mismatch');
    }
    result.entry_hash = bytesToHex(computedEntryHash);
    result.checks.push('Export receipt entry hash recomputed and matches');

    // 2. Signature (domain-separated) under the device key.
    const sigCheckable = await ed25519Supported();
    if (sigCheckable) {
      const msg = await domainSeparatedHash(V1_MANIFEST.signature_domains.export_receipt, toBytes(rEntry.entry_hash));
      const sig = Uint8Array.from(rEntry.signatures.ed25519_signature);
      if (!(await verifyEd25519(pubKey, sig, msg))) throw new Error('export receipt signature verification failed');
      result.checks.push(result.authorship === 'trusted'
        ? 'Export receipt signature valid (authorship confirmed against your trusted key)'
        : 'Export receipt signature valid under the device key named in the bundle');
    }

    // 3. Artifact binding: recompute the artifact hash exactly as the device
    //    did (serde-faithful bytes) and require it to equal the signed hash.
    const artifactHash = await sha256(utf8(serExportArtifact(bundle.artifact)));
    if (!bytesEqual(artifactHash, toBytes(rEntry.receipt.artifact_hash))) {
      throw new Error('export artifact hash mismatch');
    }
    result.artifact_hash = bytesToHex(artifactHash);
    result.checks.push('Human-readable artifact bound to the signed export hash');

    if (result.authorship !== 'trusted') {
      result.warnings.push('device key taken from the bundle itself — authorship is self-attested; compare the key against one published by the device owner, or supply it to confirm');
    }
    if (bundle.pq_public_key) result.warnings.push('post-quantum signatures present but not verified in this offline viewer');

    // Content stats (display only — the checks above are what prove them).
    for (const batch of bundle.artifact.batches || []) {
      result.batches++;
      for (const bucket of batch.buckets || []) {
        result.buckets++;
        result.events += (bucket.events || []).length;
        result.failures += (bucket.failures || []).length;
      }
    }
    if (result.failures > 0) result.warnings.push(`${result.failures} failure record(s) present (explicit gaps)`);

    // Same inconclusive semantics as the envelope path: deterministic checks
    // all passed but device authorship was not cryptographically confirmed.
    if (!sigCheckable) {
      result.status = 'inconclusive';
      result.ok = false;
      result.inconclusive = true;
      result.warnings.unshift('signatures not checked: this browser lacks Ed25519 Web Crypto support');
      result.error = 'This browser cannot check Ed25519 signatures, so device authorship was not confirmed. ' +
        'The bundle was NOT rejected — its receipt hash and the artifact-to-signed-hash binding both checked out, ' +
        'so any tampering would have been caught. To confirm the signature too, reopen this page in a current ' +
        'Chrome, Edge, Firefox, or Safari 17+, or run the securaCV Rust verifier (export_verify).';
      return result;
    }

    result.ok = true;
    result.status = result.warnings.length === 0 ? 'ok' : 'valid_with_warnings';
    return result;
  } catch (err) {
    result.ok = false;
    result.status = 'compromised';
    result.error = err.message || String(err);
    result.failure = err.failure || null;
    return result;
  }
}

const api = {
  verifyEnvelope,
  verifyExportBundle,
  looksLikeExportBundle,
  looksLikeEnvelope,
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
