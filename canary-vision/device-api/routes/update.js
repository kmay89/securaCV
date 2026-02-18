'use strict';

const { Router } = require('express');

const UPDATE_COOLDOWN_MS = 60 * 60 * 1000; // 1 hour

function updateRoutes(state) {
  const router = Router();

  router.get('/api/v1/update/check', (req, res) => {
    res.json({
      current_version: state.device.firmware_version,
      available_version: '0.4.2',
      update_available: true,
      changelog: '- Fixed WiFi reconnect bug\n- Improved motion detection',
      sha256: 'a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2',
      signature: '1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef',
      requires_usb: false,
    });
  });

  router.post('/api/v1/update', (req, res) => {
    // Check for update in progress
    if (state.getUpdateInProgress()) {
      return res.status(503).json({
        error: 'update_in_progress',
        message: 'An update is already in progress',
      });
    }

    // Rate limit: 1 per hour
    const now = Date.now();
    if (state.getLastUpdateTime() && (now - state.getLastUpdateTime()) < UPDATE_COOLDOWN_MS) {
      const retryAfter = Math.ceil((state.getLastUpdateTime() + UPDATE_COOLDOWN_MS - now) / 1000);
      res.setHeader('Retry-After', String(retryAfter));
      return res.status(429).json({
        error: 'rate_limited',
        message: 'Update rate limit exceeded. Try again later.',
        retry_after: retryAfter,
      });
    }

    // Check for firmware file
    // Since we're not using multer, check content-type for multipart
    const contentType = req.headers['content-type'] || '';
    if (!contentType.includes('multipart/form-data')) {
      return res.status(400).json({
        error: 'no_firmware',
        message: 'No firmware file provided',
      });
    }

    // Simulate reading multipart — in reference server, just accept it
    // Collect body data
    let bodySize = 0;
    req.on('data', (chunk) => {
      bodySize += chunk.length;
    });

    req.on('end', () => {
      if (bodySize === 0) {
        return res.status(400).json({
          error: 'no_firmware',
          message: 'No firmware file provided',
        });
      }

      // Simulate signature verification (always passes in reference server)
      state.setUpdateInProgress(true);
      state.setLastUpdateTime(now);
      state.addLog('WARN', 'Firmware update started');

      // Simulate update completing after a short delay
      setTimeout(() => {
        state.setUpdateInProgress(false);
        state.addLog('INFO', 'Firmware update completed');
      }, 100);

      res.json({
        ok: true,
        message: 'Updating firmware. Device will reboot.',
      });
    });
  });

  return router;
}

module.exports = updateRoutes;
