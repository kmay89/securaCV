'use strict';

const { isPrivateOrigin } = require('../lib/private-origin');

// CORS policy: same-origin only by default.
// Peer device origins are NOT allowed by default because a compromised
// peer could make cross-origin authenticated requests to any other device
// in the mesh (lateral movement vector).
//
// Two deliberate carve-outs enable one-app fleet pairing:
//  1. /api/provisioning-receipt answers CORS for any PRIVATE-network
//     origin. The endpoint is unauthenticated by design, physically
//     gated by the BOOT button, and one-shot — the browser response is
//     useless without a press.
//  2. Trust-on-pair: the origin that actually received a receipt is
//     recorded (state.addAllowedOrigin) and from then on may make
//     authenticated cross-origin calls. The physical press authorized
//     handing out the token itself; trusting its recipient origin is
//     strictly weaker.

function corsMiddleware(state) {
  return (req, res, next) => {
    const origin = req.headers.origin;
    if (!origin) {
      return next();
    }

    // Only allow the device's own origin (same-origin policy)…
    const allowedOrigins = [
      `http://${state.device.ip}`,
      `http://${state.device.mdns_hostname}`,
    ];

    // …plus origins enrolled by trust-on-pair, and any private origin
    // for the provisioning-receipt flow itself.
    const allowed =
      allowedOrigins.includes(origin) ||
      state.isOriginAllowed(origin) ||
      (req.path === '/api/provisioning-receipt' && isPrivateOrigin(origin));

    if (allowed) {
      res.setHeader('Access-Control-Allow-Origin', origin);
      res.setHeader('Access-Control-Allow-Methods', 'GET, PUT, POST, OPTIONS');
      res.setHeader('Access-Control-Allow-Headers', 'Content-Type, X-Canary-Token');
      res.setHeader('Access-Control-Max-Age', '3600');
    }

    // If it's an OPTIONS preflight (non-PNA), respond 204
    if (req.method === 'OPTIONS') {
      return res.status(204).end();
    }

    next();
  };
}

module.exports = corsMiddleware;
