'use strict';

function pnaMiddleware(state) {
  return (req, res, next) => {
    if (req.method !== 'OPTIONS') {
      return next();
    }

    const pnaRequest = req.headers['access-control-request-private-network'];
    if (pnaRequest === 'true') {
      res.setHeader('Access-Control-Allow-Private-Network', 'true');
      res.setHeader('Private-Network-Access-Name', `Canary Vision (${state.device.name})`);
      res.setHeader('Private-Network-Access-ID', state.device.mac);
      return res.status(204).end();
    }

    next();
  };
}

module.exports = pnaMiddleware;
