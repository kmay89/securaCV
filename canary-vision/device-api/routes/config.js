'use strict';

const { Router } = require('express');

const VALID_SECTIONS = ['network', 'privacy', 'detection', 'integrations'];
const VALID_PURGE_HOURS = [1, 6, 12, 24, 48, 168];

// Keys that are IMMUTABLE via the API (Invariant I: No Raw Export).
// camera_peek_enabled is a raw media export path that bypasses the kernel's
// break-glass/quorum architecture. It MUST NOT be changeable via any API
// endpoint — only via physical device interaction or break-glass quorum.
const IMMUTABLE_PRIVACY_KEYS = ['camera_peek_enabled'];

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
    if (detection && detection.suppression !== undefined) {
      const supp = detection.suppression;
      if (typeof supp !== 'object' || supp === null) {
        errors.push('suppression must be an object');
      } else {
        if (supp.enabled !== undefined && typeof supp.enabled !== 'boolean') {
          errors.push('suppression.enabled must be a boolean');
        }
        if (supp.cooldown_seconds !== undefined) {
          const c = supp.cooldown_seconds;
          if (!Number.isInteger(c) || c < 0 || c > 3600) {
            errors.push('suppression.cooldown_seconds must be an integer between 0 and 3600');
          }
        }
        if (supp.session_timeout_seconds !== undefined) {
          const t = supp.session_timeout_seconds;
          if (!Number.isInteger(t) || t < 0 || t > 600) {
            errors.push('suppression.session_timeout_seconds must be an integer between 0 and 600');
          }
        }
        if (supp.motion_cooldown_seconds !== undefined) {
          const m = supp.motion_cooldown_seconds;
          if (!Number.isInteger(m) || m < 0 || m > 3600) {
            errors.push('suppression.motion_cooldown_seconds must be an integer between 0 and 3600');
          }
        }
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

// Strip immutable keys from a privacy config update and return rejected keys.
function stripImmutableKeys(data, section) {
  const rejected = [];
  if (section === 'privacy' || (!section && data.privacy)) {
    const privacy = section ? data : data.privacy;
    if (privacy) {
      for (const key of IMMUTABLE_PRIVACY_KEYS) {
        if (key in privacy) {
          rejected.push(key);
          delete privacy[key];
        }
      }
    }
  }
  return rejected;
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

    // Enforce immutability of camera_peek_enabled via API (Invariant I).
    const rejected = stripImmutableKeys(body, null);

    // Merge each section
    for (const section of VALID_SECTIONS) {
      if (body[section] && typeof body[section] === 'object') {
        Object.assign(state.config[section], body[section]);
      }
    }

    state.addLog('INFO', 'Config updated');

    const response = { ok: true, config: structuredClone(state.config) };
    if (rejected.length > 0) {
      response.rejected_immutable = rejected;
      state.addLog('WARN', `Config update rejected immutable keys: ${rejected.join(', ')}`);
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

    // Enforce immutability of camera_peek_enabled via API (Invariant I).
    const rejected = stripImmutableKeys(body, section);

    Object.assign(state.config[section], body);
    state.addLog('INFO', `Config section '${section}' updated`);

    const response = { ok: true, config: structuredClone(state.config) };
    if (rejected.length > 0) {
      response.rejected_immutable = rejected;
      state.addLog('WARN', `Config update rejected immutable keys: ${rejected.join(', ')}`);
    }
    res.json(response);
  });

  // Physical confirm endpoint removed — camera_peek_enabled is now fully
  // immutable via the API per Invariant I (No Raw Export by Design).
  // Physical confirmation must happen via the firmware's physical button
  // mechanism, not via an HTTP endpoint.

  return router;
}

module.exports = configRoutes;
