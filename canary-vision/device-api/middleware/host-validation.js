'use strict';

function hostValidation(state) {
  const devMode = state.device.devMode || false;

  return (req, res, next) => {
    const hostHeader = req.headers.host;
    if (!hostHeader) {
      console.warn('Host validation rejected: (empty)');
      return res.status(403).json({
        error: 'host_rejected',
        message: 'Request rejected: invalid Host header',
      });
    }

    // Strip port if present
    const host = hostHeader.replace(/:\d+$/, '');

    const allowed = [
      state.device.mdns_hostname,
      state.device.ip,
    ];

    if (devMode) {
      allowed.push('localhost', '127.0.0.1');
    }

    if (allowed.includes(host)) {
      return next();
    }

    console.warn('Host validation rejected: %s', host);
    return res.status(403).json({
      error: 'host_rejected',
      message: 'Request rejected: invalid Host header',
    });
  };
}

module.exports = hostValidation;
