'use strict';

const { Router } = require('express');
const { isPrivateOrigin } = require('../lib/private-origin');

// Models the WAP's BOOT-button provisioning gate (canary_wap.ino's
// handle_provisioning_receipt) only in part: GET /api/provisioning-receipt
// is unauthenticated — it is how a client obtains the API token in the
// first place — returns 403 until the physical BOOT button is short-tapped,
// and spends one tap on one receipt. Unlike the WAP since sweep F342, it
// hands the tap to the first client that asks: the WAP serves it only to
// the sole address that asked from 10 s before the tap onward, and a second
// address closes the tap with 409 tap_contended (provisioning_gate.h's
// tap_arbitrate()), which this server does not model. The 403 body
// advertises gate_ttl_seconds so clients can show an accurate countdown.
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

    // One-shot: the first request after the tap consumes it (the WAP
    // instead closes the tap with 409 when a second address asks).
    state.consumeBootGate();
    state.addLog('INFO', 'Provisioning receipt issued');

    // Trust-on-pair: the BOOT press authorized releasing the token to
    // this client, so its (private-network) origin may make authenticated
    // cross-origin calls from now on — this is what lets one device's
    // companion app manage the whole fleet.
    const origin = req.headers.origin;
    if (origin && isPrivateOrigin(origin)) {
      state.addAllowedOrigin(origin);
    }

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
