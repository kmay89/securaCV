'use strict';

const crypto = require('node:crypto');
const { Router } = require('express');
const { computeHash, verifySignature } = require('../lib/witness-chain');
const { buildEvidenceEnvelope, EnvelopeBridgeError } = require('../lib/envelope-bridge');
const V = require('../../../viewer/verify_core');

function computeExportDigest(records) {
  const h = crypto.createHash('sha256');
  for (const r of records) {
    h.update(`${r.seq}:${r.hash}:${r.signature}\n`);
  }
  return h.digest('hex');
}

function detectSequenceGaps(records) {
  const gaps = [];
  for (let i = 1; i < records.length; i++) {
    if (records[i].seq !== records[i - 1].seq + 1) {
      gaps.push({
        after_seq: records[i - 1].seq,
        before_seq: records[i].seq,
        missing: records[i].seq - records[i - 1].seq - 1,
      });
    }
  }
  return gaps;
}

function computeTimeSourceDistribution(records) {
  const dist = {};
  for (const r of records) {
    const src = r.time_source || 'device_clock';
    dist[src] = (dist[src] || 0) + 1;
  }
  return dist;
}

function detectTimingAnomalies(records) {
  const anomalies = [];
  for (let i = 1; i < records.length; i++) {
    const prev = new Date(records[i - 1].timestamp).getTime();
    const curr = new Date(records[i].timestamp).getTime();
    if (curr < prev) {
      anomalies.push({
        seq: records[i].seq,
        type: 'clock_regression',
        delta_ms: curr - prev,
      });
    }
  }
  return anomalies;
}

// Dedicated, stricter rate limiter for the crypto-heavy endpoints. Building/verifying an evidence
// envelope runs Ed25519 sign+verify and SHA-256 over every record, so these routes are far more
// expensive than a plain read and warrant their own cap on top of the global limiter in server.js.
// Kept dependency-free and in the same in-house style as middleware/rate-limit.js (the project
// intentionally ships with only express). Per-IP sliding window with bounded, LRU-evicted state.
// Config is read from `state.envelopeRateLimit` at request time so it stays overridable in tests.
function expensiveRouteLimiter(state) {
  const maxEntries = 64;
  const hits = new Map(); // ip -> number[] (timestamps)

  return function limiter(req, res, next) {
    const cfg = (state && state.envelopeRateLimit) || {};
    const limit = cfg.limit || 6;
    const windowMs = cfg.windowMs || 60000;
    const ip = req.ip || (req.socket && req.socket.remoteAddress) || 'unknown';
    const now = Date.now();
    let arr = hits.get(ip);
    if (!arr) {
      if (hits.size >= maxEntries) hits.delete(hits.keys().next().value); // LRU evict
      arr = [];
    } else {
      hits.delete(ip); // re-insert to mark most-recently-used
    }
    while (arr.length > 0 && arr[0] <= now - windowMs) arr.shift();
    if (arr.length >= limit) {
      const retryAfter = Math.ceil((arr[0] + windowMs - now) / 1000);
      res.setHeader('Retry-After', String(retryAfter));
      hits.set(ip, arr);
      return res.status(429).json({
        error: 'rate_limited',
        message: 'Too many evidence-verification requests. Please slow down.',
        retry_after: retryAfter,
      });
    }
    arr.push(now);
    hits.set(ip, arr);
    return next();
  };
}

function witnessRoutes(state) {
  const router = Router();

  // Stricter cap for the Ed25519/SHA-256-heavy envelope + verify endpoints (DoS hardening).
  const envelopeLimiter = expensiveRouteLimiter(state);

  router.get('/api/v1/witness', (req, res) => {
    let last = parseInt(req.query.last, 10) || 20;
    last = Math.min(Math.max(1, last), 100);

    res.json({
      records: state.witnessRecords.slice(-last),
    });
  });

  router.get('/api/v1/witness/export', (req, res) => {
    const pubKeyPem = state.publicKey.export({ type: 'spki', format: 'pem' });
    const pubKeyDer = state.publicKey.export({ type: 'spki', format: 'der' });
    const fingerprint = crypto.createHash('sha256').update(pubKeyDer).digest('hex');
    const records = state.witnessRecords;
    const exportedAt = new Date().toISOString();

    const envelope = {
      format: 'securacv-witness-chain-v1',
      exported_at: exportedAt,

      custodian: {
        device_id: state.device.device_id,
        device_name: state.device.name,
        firmware_version: state.device.firmware_version,
        public_key_fingerprint: fingerprint,
        public_key_algorithm: 'Ed25519',
        hash_algorithm: 'SHA-256',
      },

      chain_summary: {
        record_count: records.length,
        first_seq: records.length > 0 ? records[0].seq : null,
        last_seq: records.length > 0 ? records[records.length - 1].seq : null,
        first_timestamp: records.length > 0 ? records[0].timestamp : null,
        last_timestamp: records.length > 0 ? records[records.length - 1].timestamp : null,
        sequence_gaps: detectSequenceGaps(records),
        timing_anomalies: detectTimingAnomalies(records),
        time_source_distribution: computeTimeSourceDistribution(records),
      },

      public_key_pem: pubKeyPem,

      records,

      export_digest: computeExportDigest(records),

      verification_instructions: {
        description: 'To independently verify this chain: '
          + '(1) Recompute each record hash as SHA-256("seq:prev_hash:timestamp:event_type:zone:time_source:gps_timestamp"). '
          + '(2) Verify prev_hash links to the preceding record. '
          + '(3) Verify each Ed25519 signature against the hash using the public key above. '
          + '(4) Recompute export_digest as SHA-256 of all "seq:hash:signature\\n" concatenated. '
          + '(5) Check time_source per record: gps_utc timestamps are satellite-derived (no internet), '
          + 'device_clock timestamps are from the local system clock.',
        time_sources: {
          gps_utc: 'UTC time from GNSS satellite signal (atomic clock accuracy, internet-independent)',
          device_clock: 'Local device clock (may drift without GPS or NTP sync)',
        },
        tools: 'openssl, node:crypto, or any Ed25519 + SHA-256 implementation',
      },
    };

    res.setHeader('Content-Type', 'application/json');
    res.setHeader('Content-Disposition',
      `attachment; filename="witness-chain-${state.device.device_id}-${exportedAt.slice(0, 10)}.json"`);

    res.json(envelope);
  });

  // Canonical evidence envelope: the privacy-coarsened, self-verifying interchange format
  // (securacv-evidence-envelope v1). Unlike /export (the raw witness-chain-v1 dump), this strips
  // precise timestamps / GPS / thumbnails and can be reviewed in viewer/evidence_viewer.html or
  // verified by the Rust envelope_verify CLI. The bridge fails closed if any forbidden field would
  // leak, surfaced here as HTTP 422 so the operator knows the raw chain still holds precise data.
  router.get('/api/v1/witness/envelope', envelopeLimiter, async (req, res) => {
    let bucketS = parseInt(req.query.bucket_s, 10);
    if (!Number.isInteger(bucketS)) bucketS = undefined;
    try {
      const envelope = await buildEvidenceEnvelope({
        records: state.witnessRecords,
        publicKey: state.publicKey,
        privateKey: state.privateKey,
        device: state.device,
        bucketS,
      });
      const exportedAt = new Date().toISOString();
      res.setHeader('Content-Type', 'application/json');
      res.setHeader('Content-Disposition',
        `attachment; filename="evidence-envelope-${state.device.device_id}-${exportedAt.slice(0, 10)}.json"`);
      res.json(envelope);
    } catch (err) {
      if (err instanceof EnvelopeBridgeError) {
        state.addLog('WARN', `Evidence envelope refused: ${err.message}`);
        return res.status(422).json({ error: 'coarsening_failed', message: err.message });
      }
      state.addLog('ERROR', `Evidence envelope build failed: ${err.message}`);
      return res.status(500).json({ error: 'envelope_build_failed', message: err.message });
    }
  });

  router.post('/api/v1/witness/stream/ticket', (req, res) => {
    const ticket = state.issueSseTicket();
    res.json({ ticket, expires_in_seconds: 30 });
  });

  router.get('/api/v1/witness/stream', (req, res) => {
    const ticket = req.query.ticket;
    if (!ticket || !state.consumeSseTicket(ticket)) {
      return res.status(401).json({
        error: 'ticket_invalid',
        message: 'Valid SSE ticket required. Obtain one via POST /api/v1/witness/stream/ticket',
      });
    }

    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
      'X-Accel-Buffering': 'no',
    });

    res.write('event: connected\ndata: {"status":"connected"}\n\n');

    const heartbeatInterval = setInterval(() => {
      res.write(':heartbeat\n\n');
    }, 15000);

    const sseClients = state._sseClients || (state._sseClients = new Set());
    const client = { res };
    sseClients.add(client);

    if (!state._sseHooked) {
      state._sseHooked = true;
      state._prevCb = state.getOnWitnessRecord();
      state.setOnWitnessRecord((record) => {
        if (state._prevCb) state._prevCb(record);
        for (const c of sseClients) {
          try {
            c.res.write('event: witness\ndata: ' + JSON.stringify(record) + '\n\n');
          } catch { /* client disconnected */ }
        }
      });
    }

    req.on('close', () => {
      clearInterval(heartbeatInterval);
      sseClients.delete(client);
      if (sseClients.size === 0 && state._sseHooked) {
        state.setOnWitnessRecord(state._prevCb);
        state._sseHooked = false;
        state._prevCb = null;
      }
    });
  });

  router.post('/api/v1/witness/verify', envelopeLimiter, async (req, res) => {
    const records = state.witnessRecords;
    const results = {
      total: records.length,
      valid: 0,
      hash_errors: [],
      chain_errors: [],
      signature_errors: [],
      sequence_gaps: detectSequenceGaps(records),
      timing_anomalies: detectTimingAnomalies(records),
    };

    for (let i = 0; i < records.length; i++) {
      const r = records[i];
      const expectedPrev = i > 0 ? records[i - 1].hash : '0'.repeat(64);
      const expectedHash = computeHash(r.seq, expectedPrev, r.timestamp, r.event_type, r.zone,
                                       r.time_source, r.gps_timestamp);

      let valid = true;

      if (r.hash !== expectedHash) {
        results.hash_errors.push({ seq: r.seq, expected: expectedHash, got: r.hash });
        valid = false;
      }

      if (r.prev_hash !== expectedPrev) {
        results.chain_errors.push({ seq: r.seq, expected: expectedPrev, got: r.prev_hash });
        valid = false;
      }

      try {
        if (!verifySignature(r.hash, r.signature, state.publicKey)) {
          results.signature_errors.push({ seq: r.seq });
          valid = false;
        }
      } catch {
        results.signature_errors.push({ seq: r.seq });
        valid = false;
      }

      if (valid) results.valid++;
    }

    const has_gaps = results.sequence_gaps.length > 0;
    const has_anomalies = results.timing_anomalies.length > 0;
    const all_valid = results.valid === results.total;

    if (all_valid && !has_gaps && !has_anomalies) {
      results.integrity = 'ok';
    } else if (all_valid && (has_gaps || has_anomalies)) {
      results.integrity = 'valid_with_warnings';
    } else {
      results.integrity = 'compromised';
    }

    // Also verify the canonical evidence envelope using the SAME verifier the offline viewer runs
    // (viewer/verify_core.js) — so "the device says it's intact" and "an independent reviewer says
    // it's intact" are produced by one implementation, not two that could diverge. This is
    // best-effort: a coarsening refusal (precise data still in the raw chain) is reported, not fatal.
    try {
      const envelope = await buildEvidenceEnvelope({
        records,
        publicKey: state.publicKey,
        privateKey: state.privateKey,
        device: state.device,
      });
      const report = await V.verifyEnvelope(envelope);
      results.evidence_envelope = {
        status: report.status,
        ok: report.ok,
        whole_envelope_digest: report.whole_envelope_digest,
        sealed_events: report.sealed_events,
        checks: report.checks,
        warnings: report.warnings,
        error: report.error,
      };
    } catch (err) {
      results.evidence_envelope = {
        status: err instanceof EnvelopeBridgeError ? 'coarsening_failed' : 'error',
        ok: false,
        error: err.message,
      };
    }

    state.addLog('INFO', `Witness chain verified: ${results.valid}/${results.total} valid, ` +
      `${results.sequence_gaps.length} gaps, ${results.timing_anomalies.length} anomalies`);
    res.json(results);
  });

  return router;
}

module.exports = witnessRoutes;
