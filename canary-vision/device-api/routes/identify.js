'use strict';

const { Router } = require('express');

const IDENTIFY_DEFAULT_MS = 15000;
const IDENTIFY_MIN_MS = 1000;
const IDENTIFY_MAX_MS = 60000;

// Mirrors the firmware's POST /api/identify: blink the LED (and chirp the
// buzzer when present) so the user can physically locate this device.
// The reference server has no LED, so visual_only is always true.
function identifyRoutes(state) {
  const router = Router();

  router.post('/api/identify', (req, res) => {
    let durationMs = req.body ? req.body.duration_ms : undefined;
    durationMs = Number.isFinite(durationMs)
      ? Math.min(Math.max(durationMs, IDENTIFY_MIN_MS), IDENTIFY_MAX_MS)
      : IDENTIFY_DEFAULT_MS;

    state.addLog('INFO', `Identify requested: blinking for ${durationMs}ms`);

    res.json({
      ok: true,
      duration_ms: durationMs,
      visual_only: true,
    });
  });

  return router;
}

module.exports = identifyRoutes;
