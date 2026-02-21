'use strict';

const { validateToken } = require('../lib/token');

// Server-side session TTL: 8 hours default, configurable via env.
// Evaluated lazily so env changes take effect without module reload.
function getSessionTtlMs() {
  const hours = parseFloat(process.env.SECURACV_SESSION_TTL_HOURS);
  return (isNaN(hours) || hours <= 0 ? 8 : hours) * 3600 * 1000;
}

// Track when each token was first used. Map<tokenHash, firstSeenMs>.
// Using a hash of the token to avoid storing plaintext tokens in memory.
const crypto = require('node:crypto');
const sessionFirstSeen = new Map();

function tokenHash(token) {
  return crypto.createHash('sha256').update(token).digest('hex');
}

function authMiddleware(state) {
  return (req, res, next) => {
    const token = req.headers['x-canary-token'];

    if (token === undefined || token === null) {
      if (typeof res.recordAuthFailure === 'function') {
        res.recordAuthFailure();
      }
      return res.status(401).json({
        error: 'token_required',
        message: 'X-Canary-Token header is required',
      });
    }

    if (!validateToken(token, state.device.api_token)) {
      if (typeof res.recordAuthFailure === 'function') {
        res.recordAuthFailure();
      }
      return res.status(401).json({
        error: 'token_invalid',
        message: 'Invalid API token',
      });
    }

    // Server-side session expiration check
    const hash = tokenHash(token);
    const now = Date.now();
    if (!sessionFirstSeen.has(hash)) {
      sessionFirstSeen.set(hash, now);
    }
    const firstSeen = sessionFirstSeen.get(hash);
    if (now - firstSeen > getSessionTtlMs()) {
      // Expire the session
      sessionFirstSeen.delete(hash);
      return res.status(401).json({
        error: 'token_expired',
        message: 'Session expired. Please re-authenticate.',
      });
    }

    // Successful auth — reset lockout escalation
    if (typeof res.recordAuthSuccess === 'function') {
      res.recordAuthSuccess();
    }

    next();
  };
}

module.exports = authMiddleware;
