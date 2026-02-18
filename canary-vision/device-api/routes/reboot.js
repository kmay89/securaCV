'use strict';

const { Router } = require('express');

const REBOOT_COOLDOWN_MS = 5 * 60 * 1000; // 5 minutes

function rebootRoutes(state) {
  const router = Router();

  router.post('/api/v1/reboot', (req, res) => {
    const now = Date.now();
    if (state.getRebootTime() && (now - state.getRebootTime()) < REBOOT_COOLDOWN_MS) {
      const retryAfter = Math.ceil((state.getRebootTime() + REBOOT_COOLDOWN_MS - now) / 1000);
      res.setHeader('Retry-After', String(retryAfter));
      return res.status(429).json({
        error: 'rate_limited',
        message: 'Reboot rate limit exceeded. Try again later.',
        retry_after: retryAfter,
      });
    }

    state.setRebootTime(now);
    state.addLog('WARN', 'Reboot requested');

    res.json({
      ok: true,
      message: 'Rebooting in 2 seconds',
    });
  });

  return router;
}

module.exports = rebootRoutes;
