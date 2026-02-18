'use strict';

const path = require('node:path');
const express = require('express');
const { createDeviceState } = require('./lib/device-state');
const securityHeaders = require('./middleware/security-headers');
const hostValidation = require('./middleware/host-validation');
const rateLimit = require('./middleware/rate-limit');
const pnaMiddleware = require('./middleware/pna');
const corsMiddleware = require('./middleware/cors');
const authMiddleware = require('./middleware/auth');
const infoRoutes = require('./routes/info');
const configRoutes = require('./routes/config');
const logsRoutes = require('./routes/logs');
const witnessRoutes = require('./routes/witness');
const peersRoutes = require('./routes/peers');
const rebootRoutes = require('./routes/reboot');
const updateRoutes = require('./routes/update');

function createApp(options = {}) {
  const state = createDeviceState({
    devMode: options.devMode !== undefined ? options.devMode : true,
    peers: options.peers,
    ...options.deviceOverrides,
  });

  const app = express();

  // Trust proxy for rate limiting IP extraction
  app.set('trust proxy', false);

  // Disable X-Powered-By
  app.disable('x-powered-by');

  // --- Middleware stack (ORDER MATTERS) ---

  // 1. Security headers on ALL responses
  app.use(securityHeaders());

  // 2. Host header validation (403 for bad hosts)
  app.use(hostValidation(state));

  // 3. Rate limiting (429)
  const rateLimitMiddleware = rateLimit(options.rateLimit || {});
  app.use(rateLimitMiddleware);

  // 4. Static file serving (NO auth required)
  const spaDir = options.spaDir || path.join(__dirname, '..', 'spa');
  app.use(express.static(spaDir));

  // 5. PNA preflight handling
  app.use(pnaMiddleware(state));

  // 6. CORS (peer-only)
  app.use(corsMiddleware(state));

  // 7. JSON body parsing for API routes
  app.use('/api', express.json());

  // 8. Auth (401) — applies to all /api/* routes
  app.use('/api', authMiddleware(state));

  // 9. Route handlers
  app.use(infoRoutes(state));
  app.use(configRoutes(state));
  app.use(logsRoutes(state));
  app.use(witnessRoutes(state));
  app.use(peersRoutes(state));
  app.use(rebootRoutes(state));
  app.use(updateRoutes(state));

  // 404 for unmatched API routes
  app.use('/api', (req, res) => {
    res.status(404).json({ error: 'not_found', message: 'Endpoint not found' });
  });

  return { app, state, rateLimitMiddleware };
}

// Run as standalone server
if (require.main === module) {
  const port = process.env.PORT || 3000;
  const { app } = createApp({ devMode: true });
  app.listen(port, () => {
    console.log(`Canary Vision device-api listening on http://localhost:${port}`);
    console.log('Dev mode enabled (localhost allowed in Host validation)');
  });
}

module.exports = { createApp };
