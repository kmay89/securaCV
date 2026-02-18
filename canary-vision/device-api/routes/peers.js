'use strict';

const { Router } = require('express');

function peersRoutes(state) {
  const router = Router();

  router.get('/api/v1/peers', (req, res) => {
    res.json({
      peers: state.peers,
    });
  });

  return router;
}

module.exports = peersRoutes;
