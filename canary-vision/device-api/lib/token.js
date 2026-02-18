'use strict';

const crypto = require('node:crypto');

/**
 * Constant-time token comparison to prevent timing attacks.
 */
function validateToken(provided, expected) {
  if (!provided || typeof provided !== 'string') return false;
  if (!expected || typeof expected !== 'string') return false;

  // Normalize to buffers of equal length for timingSafeEqual
  const providedBuf = Buffer.from(provided, 'utf8');
  const expectedBuf = Buffer.from(expected, 'utf8');

  if (providedBuf.length !== expectedBuf.length) {
    // Still do a constant-time comparison to avoid leaking length info
    // Compare expected against itself to consume the same time
    crypto.timingSafeEqual(expectedBuf, expectedBuf);
    return false;
  }

  return crypto.timingSafeEqual(providedBuf, expectedBuf);
}

module.exports = { validateToken };
