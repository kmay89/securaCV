'use strict';

const crypto = require('node:crypto');
const { Router } = require('express');
const { computeHash, verifySignature } = require('../lib/witness-chain');

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

function witnessRoutes(state) {
  const router = Router();

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
          + '(1) Recompute each record hash as SHA-256("seq:prev_hash:timestamp:event_type:zone"). '
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

  router.post('/api/v1/witness/verify', (req, res) => {
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
      const expectedHash = computeHash(r.seq, expectedPrev, r.timestamp, r.event_type, r.zone);

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

    state.addLog('INFO', `Witness chain verified: ${results.valid}/${results.total} valid, ` +
      `${results.sequence_gaps.length} gaps, ${results.timing_anomalies.length} anomalies`);
    res.json(results);
  });

  let lastPurgeTime = 0;

  router.delete('/api/v1/witness', (req, res) => {
    if (req.query.confirm !== 'true') {
      return res.status(400).json({
        error: 'confirmation_required',
        message: 'Add ?confirm=true to purge all witness records. This is irreversible.',
      });
    }

    const now = Date.now();
    if (now - lastPurgeTime < 300000) {
      return res.status(429).json({
        error: 'rate_limited',
        message: 'Purge allowed once per 5 minutes.',
      });
    }
    lastPurgeTime = now;

    const count = state.witnessRecords.length;
    state.witnessRecords.length = 0;
    state.addLog('WARN', `Witness chain purged (${count} records deleted)`);

    res.json({ ok: true, purged: count });
  });

  return router;
}

module.exports = witnessRoutes;
