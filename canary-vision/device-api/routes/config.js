'use strict';

const { Router } = require('express');

const VALID_SECTIONS = ['network', 'privacy', 'detection', 'integrations'];
const VALID_PURGE_HOURS = [1, 6, 12, 24, 48, 168];

function validateConfigValues(section, data) {
  const errors = [];

  if (section === 'privacy' || (!section && data.privacy)) {
    const privacy = section ? data : data.privacy;
    if (privacy && privacy.auto_purge_hours !== undefined) {
      if (!VALID_PURGE_HOURS.includes(privacy.auto_purge_hours)) {
        errors.push(`auto_purge_hours must be one of: ${VALID_PURGE_HOURS.join(', ')}`);
      }
    }
  }

  if (section === 'detection' || (!section && data.detection)) {
    const detection = section ? data : data.detection;
    if (detection && detection.motion_sensitivity !== undefined) {
      const s = detection.motion_sensitivity;
      if (!Number.isInteger(s) || s < 1 || s > 10) {
        errors.push('motion_sensitivity must be an integer between 1 and 10');
      }
    }
  }

  if (section === 'network' || (!section && data.network)) {
    const network = section ? data : data.network;
    if (network) {
      if (network.mqtt_port !== undefined) {
        const p = network.mqtt_port;
        if (!Number.isInteger(p) || p < 1 || p > 65535) {
          errors.push('mqtt_port must be an integer between 1 and 65535');
        }
      }
      if (network.wifi_ssid !== undefined) {
        if (typeof network.wifi_ssid !== 'string' || network.wifi_ssid.length === 0) {
          errors.push('wifi_ssid must be a non-empty string');
        }
      }
    }
  }

  return errors;
}

function configRoutes(state) {
  const router = Router();

  router.get('/api/v1/config', (req, res) => {
    res.json(structuredClone(state.config));
  });

  router.put('/api/v1/config', (req, res) => {
    const body = req.body;
    if (!body || typeof body !== 'object') {
      return res.status(400).json({
        error: 'invalid_config',
        message: 'Request body must be a JSON object',
      });
    }

    // Validate across all sections
    const errors = validateConfigValues(null, body);
    if (errors.length > 0) {
      return res.status(400).json({
        error: 'invalid_config',
        message: errors.join('; '),
      });
    }

    const pendingConfirm = [];

    // Merge each section
    for (const section of VALID_SECTIONS) {
      if (body[section] && typeof body[section] === 'object') {
        // Check for camera_peek_enabled requiring physical confirmation
        if (section === 'privacy' && body.privacy.camera_peek_enabled === true) {
          pendingConfirm.push('camera_peek_enabled');
          state.pendingPhysicalConfirm.add('camera_peek_enabled');
          delete body.privacy.camera_peek_enabled;
        }

        Object.assign(state.config[section], body[section]);
      }
    }

    state.addLog('INFO', 'Config updated');

    const response = { ok: true, config: structuredClone(state.config) };
    if (pendingConfirm.length > 0) {
      response.pending_physical_confirm = pendingConfirm;
    }
    res.json(response);
  });

  router.get('/api/v1/config/:section', (req, res) => {
    const section = req.params.section;
    if (!VALID_SECTIONS.includes(section)) {
      return res.status(404).json({
        error: 'not_found',
        message: `Unknown config section: ${section}`,
      });
    }
    res.json(structuredClone(state.config[section]));
  });

  router.put('/api/v1/config/:section', (req, res) => {
    const section = req.params.section;
    if (!VALID_SECTIONS.includes(section)) {
      return res.status(404).json({
        error: 'not_found',
        message: `Unknown config section: ${section}`,
      });
    }

    const body = req.body;
    if (!body || typeof body !== 'object') {
      return res.status(400).json({
        error: 'invalid_config',
        message: 'Request body must be a JSON object',
      });
    }

    const errors = validateConfigValues(section, body);
    if (errors.length > 0) {
      return res.status(400).json({
        error: 'invalid_config',
        message: errors.join('; '),
      });
    }

    const pendingConfirm = [];

    if (section === 'privacy' && body.camera_peek_enabled === true) {
      pendingConfirm.push('camera_peek_enabled');
      state.pendingPhysicalConfirm.add('camera_peek_enabled');
      delete body.camera_peek_enabled;
    }

    Object.assign(state.config[section], body);
    state.addLog('INFO', `Config section '${section}' updated`);

    const response = { ok: true, config: structuredClone(state.config) };
    if (pendingConfirm.length > 0) {
      response.pending_physical_confirm = pendingConfirm;
    }
    res.json(response);
  });

  // Physical confirm endpoint (simulates button press)
  router.post('/api/v1/confirm', (req, res) => {
    if (state.pendingPhysicalConfirm.size === 0) {
      return res.status(400).json({
        error: 'nothing_pending',
        message: 'No settings pending physical confirmation',
      });
    }

    for (const key of state.pendingPhysicalConfirm) {
      if (key === 'camera_peek_enabled') {
        state.config.privacy.camera_peek_enabled = true;
      }
    }
    state.pendingPhysicalConfirm.clear();
    state.addLog('INFO', 'Physical confirmation received');

    res.json({ ok: true, confirmed: ['camera_peek_enabled'] });
  });

  return router;
}

module.exports = configRoutes;
