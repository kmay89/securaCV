'use strict';

const crypto = require('node:crypto');

/**
 * Computes SHA-256 hash for a witness record.
 */
function computeHash(seq, prevHash, timestamp, eventType, zone, timeSource, gpsTimestamp) {
  const ts = timeSource || 'device_clock';
  const gps = gpsTimestamp || '';
  const data = `${seq}:${prevHash}:${timestamp}:${eventType}:${zone}:${ts}:${gps}`;
  return crypto.createHash('sha256').update(data).digest('hex');
}

/**
 * Signs a hash with Ed25519 private key.
 */
function signHash(hash, privateKey) {
  return crypto.sign(undefined, Buffer.from(hash), privateKey).toString('hex');
}

/**
 * Verifies an Ed25519 signature.
 */
function verifySignature(hash, signature, publicKey) {
  return crypto.verify(undefined, Buffer.from(hash), publicKey, Buffer.from(signature, 'hex'));
}

module.exports = { computeHash, signHash, verifySignature };
