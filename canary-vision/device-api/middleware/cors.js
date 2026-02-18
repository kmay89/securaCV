'use strict';

function corsMiddleware(state) {
  return (req, res, next) => {
    const origin = req.headers.origin;
    if (!origin) {
      return next();
    }

    // Build list of allowed peer origins
    const allowedOrigins = state.peers.map((p) => {
      return [`http://${p.ip}`, `http://${p.device_id}.local`];
    }).flat();

    // Also allow the device's own origin
    allowedOrigins.push(
      `http://${state.device.ip}`,
      `http://${state.device.mdns_hostname}`
    );

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
