'use strict';

const crypto = require('node:crypto');

/**
 * Computes SHA-256 hash for a witness record.
 */
function computeHash(seq, prevHash, timestamp, eventType, zone) {
  const data = `${seq}:${prevHash}:${timestamp}:${eventType}:${zone}`;
  return crypto.createHash('sha256').update(data).digest('hex');
}

/**
 * Signs a hash with Ed25519 private key.
 */
function signHash(hash, privateKey) {
  return crypto.sign(null, Buffer.from(hash), privateKey).toString('hex');
}

/**
 * Verifies an Ed25519 signature.
 */
function verifySignature(hash, signature, publicKey) {
  return crypto.verify(null, Buffer.from(hash), publicKey, Buffer.from(signature, 'hex'));
}

module.exports = { computeHash, signHash, verifySignature };
