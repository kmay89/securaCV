'use strict';

const { validateToken } = require('../lib/token');

function authMiddleware(state) {
  return (req, res, next) => {
    const token = req.headers['x-canary-token'];

    if (token === undefined || token === null) {
      if (typeof res.recordAuthFailure === 'function') {
        res.recordAuthFailure();
      }
      return res.status(401).json({
        error: 'token_required',
        message: 'X-Canary-Token header is required',
      });
    }

    if (!validateToken(token, state.device.api_token)) {
      if (typeof res.recordAuthFailure === 'function') {
        res.recordAuthFailure();
      }
      return res.status(401).json({
        error: 'token_invalid',
        message: 'Invalid API token',
      });
    }

    // Successful auth — reset lockout escalation
    if (typeof res.recordAuthSuccess === 'function') {
      res.recordAuthSuccess();
    }

    next();
  };
}

module.exports = authMiddleware;
