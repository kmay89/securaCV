'use strict';

const { Router } = require('express');

const NAME_MAX_LEN = 32;

// Mirrors the firmware's POST /api/device-name: rename the device.
// Names are sanitized to [a-z0-9-] per DNS label rules, and the mDNS host
// becomes canary-<name> (re-announced immediately, no reboot).
function deviceNameRoutes(state) {
  const router = Router();

  router.post('/api/device-name', (req, res) => {
    const raw = req.body ? req.body.name : undefined;
    if (typeof raw !== 'string' || raw.trim().length === 0) {
      return res.status(400).json({
        error: 'invalid_name',
        message: 'name is required',
      });
    }

    const name = raw
      .trim()
      .toLowerCase()
      .replace(/[^a-z0-9-]+/g, '-')
      .slice(0, NAME_MAX_LEN)
      // Trim edge hyphens AFTER slicing — truncation can land on a
      // separator, and a label ending in '-' is not valid DNS.
      .replace(/^-+|-+$/g, '');

    if (name.length === 0) {
      return res.status(400).json({
        error: 'invalid_name',
        message: 'name must contain at least one of a-z, 0-9, or -',
      });
    }

    state.setDeviceName(name);
    state.addLog('INFO', `Device renamed to "${name}" (canary-${name}.local)`);

    res.json({
      ok: true,
      device_name: name,
      mdns_host: 'canary-' + name,
    });
  });

  return router;
}

module.exports = deviceNameRoutes;
