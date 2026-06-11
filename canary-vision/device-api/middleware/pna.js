'use strict';

const crypto = require('node:crypto');

/**
 * SECURITY: Derive a stable, non-reversible identifier from the MAC address.
 * Exposing raw MAC addresses in HTTP headers aids device fingerprinting and
 * tracking. The HMAC-SHA256 truncation produces a stable ID per device
 * (browsers can still distinguish devices) without leaking the real MAC.
 */
function hashMac(mac) {
  return crypto
    .createHmac('sha256', 'securacv-pna-id-v1')
    .update(mac)
    .digest('hex')
    .slice(0, 16);
}

function pnaMiddleware(state) {
  // Pre-compute the hashed ID once at startup
  const hashedId = hashMac(state.device.mac);

  return (req, res, next) => {
    if (req.method !== 'OPTIONS') {
      return next();
    }

    const pnaRequest = req.headers['access-control-request-private-network'];
    if (pnaRequest === 'true') {
      res.setHeader('Access-Control-Allow-Private-Network', 'true');
      res.setHeader('Private-Network-Access-Name', `Canary Vision (${state.device.name})`);
      res.setHeader('Private-Network-Access-ID', hashedId);

      // Chrome's PNA preflight is still a CORS preflight: without the
      // Access-Control-Allow-* grants the browser rejects it even when
      // the PNA header is present. When the request carries an Origin,
      // fall through to the CORS middleware so allowed origins get both
      // sets of headers in one response.
      if (req.headers.origin) {
        return next();
      }
      return res.status(204).end();
    }

    next();
  };
}

module.exports = pnaMiddleware;
