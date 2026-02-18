'use strict';

const { Router } = require('express');

function witnessRoutes(state) {
  const router = Router();

  router.get('/api/v1/witness', (req, res) => {
    let last = parseInt(req.query.last, 10) || 20;
    last = Math.min(Math.max(1, last), 100);

    res.json({
      records: state.witnessRecords.slice(-last),
    });
  });

  return router;
}

module.exports = witnessRoutes;
