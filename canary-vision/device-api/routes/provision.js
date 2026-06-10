'use strict';

const { Router } = require('express');

// Mirrors the firmware's BOOT-button provisioning gate (wap_server.h):
// GET /api/provisioning-receipt is unauthenticated — it is how a client
// obtains the API token in the first place — but returns 403 until the
// physical BOOT button is short-tapped. The 403 body advertises
// gate_ttl_seconds so clients can show an accurate countdown.
function provisionRoutes(state) {
  const router = Router();

  router.get('/api/provisioning-receipt', (req, res) => {
    if (!state.isBootGateOpen()) {
      return res.status(403).json({
        error: 'gate_closed',
        message: 'Short-tap the BOOT button on the device to unlock the provisioning receipt.',
        gate_ttl_seconds: state.BOOT_GATE_TTL_SECONDS,
      });
    }

    // One-shot: consume the gate on success so a second client polling in
    // the same window cannot also capture the token.
    state.consumeBootGate();
    state.addLog('INFO', 'Provisioning receipt issued');

    res.json({
      device_id: state.device.device_id,
      base_url: `http://${req.headers.host}`,
      token: state.device.api_token,
    });
  });

  // Dev-only stand-in for the physical button. The real device opens the
  // gate from a GPIO interrupt; the reference server opens it from this
  // endpoint so the pairing flow can be exercised end-to-end locally.
  router.post('/api/dev/press-boot', (req, res) => {
    if (!state.device.devMode) {
      return res.status(404).json({ error: 'not_found', message: 'Endpoint not found' });
    }
    state.openBootGate();
    res.json({ ok: true, gate_ttl_seconds: state.BOOT_GATE_TTL_SECONDS });
  });

  return router;
}

module.exports = provisionRoutes;
