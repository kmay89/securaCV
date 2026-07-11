'use strict';

const { Router } = require('express');

function infoRoutes(state) {
  const router = Router();

  router.get('/api/v1/info', (req, res) => {
    res.json({
      device_id: state.device.device_id,
      name: state.device.name,
      model: state.device.model,
      device_type: state.device.device_type,
      firmware_version: state.device.firmware_version,
      uptime_s: state.getUptime(),
      last_seen: new Date().toISOString(),
      wifi_rssi: -42,
      ip: state.device.ip,
      capabilities: ['camera_peek', 'ota_update', 'mqtt', 'witness_log'],
    });
  });

  return router;
}

module.exports = infoRoutes;
