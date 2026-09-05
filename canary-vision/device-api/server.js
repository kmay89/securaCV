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
const webhookRoutes = require('./routes/webhook');
const provisionRoutes = require('./routes/provision');
const identifyRoutes = require('./routes/identify');
const deviceNameRoutes = require('./routes/device-name');
const { createWebhookDispatcher } = require('./lib/webhook');

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

  // 8a. Provisioning receipt (NO token auth — it is how a client obtains
  // the token; gated by the physical BOOT button instead)
  app.use(provisionRoutes(state));

  // 8b. Auth (401) — applies to all other /api/* routes
  app.use('/api', authMiddleware(state));

  // 9. Webhook dispatcher
  const webhookDispatcher = createWebhookDispatcher(state);
  state.setOnWitnessRecord((record) => {
    webhookDispatcher.dispatch('witness_event', record);
  });

  // 10. Route handlers
  app.use(infoRoutes(state));
  app.use(configRoutes(state));
  app.use(logsRoutes(state));
  app.use(witnessRoutes(state));
  app.use(peersRoutes(state));
  app.use(rebootRoutes(state));
  app.use(updateRoutes(state));
  app.use(webhookRoutes(state, webhookDispatcher));
  app.use(identifyRoutes(state));
  app.use(deviceNameRoutes(state));

  // 404 for unmatched API routes
  app.use('/api', (req, res) => {
    res.status(404).json({ error: 'not_found', message: 'Endpoint not found' });
  });

  return { app, state, rateLimitMiddleware };
}

// Run as standalone server
if (require.main === module) {
  const fs = require('node:fs');
  const port = process.env.PORT || 3000;
  // Dev-mode server: Host validation admits localhost, so bind loopback unless
  // the operator says otherwise. `app.listen(port)` alone listens on every
  // interface while the line below claims "localhost".
  const host = process.env.HOST || '127.0.0.1';
  const dataDir = process.env.CANARY_DATA_DIR || path.join(__dirname, '..', '.data');
  const { app, state } = createApp({ devMode: true, deviceOverrides: { keyDir: dataDir } });
  app.listen(port, host, () => {
    console.log(`Canary Vision device-api listening on http://${host}:${port}`);
    console.log('Dev mode enabled (localhost allowed in Host validation)');
    // Write token to file instead of stdout to prevent log exposure
    const tokenPath = path.join(dataDir, 'api_token');
    fs.mkdirSync(dataDir, { recursive: true });
    fs.writeFileSync(tokenPath, state.device.api_token + '\n', { mode: 0o600 });
    console.log(`API token written to ${tokenPath}`);
  });
}

module.exports = { createApp };
