'use strict';

// CORS policy: same-origin only by default.
// Peer device origins are NOT allowed because a compromised peer could make
// cross-origin authenticated requests to any other device in the mesh
// (lateral movement vector). Cross-device API calls should use a separate
// auth mechanism (device-to-device mutual TLS or signed requests).

function corsMiddleware(state) {
  return (req, res, next) => {
    const origin = req.headers.origin;
    if (!origin) {
      return next();
    }

    // Only allow the device's own origin (same-origin policy)
    const allowedOrigins = [
      `http://${state.device.ip}`,
      `http://${state.device.mdns_hostname}`,
    ];

    if (allowedOrigins.includes(origin)) {
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
