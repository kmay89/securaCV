'use strict';

const { Router } = require('express');

function logsRoutes(state) {
  const router = Router();

  router.get('/api/v1/logs', (req, res) => {
    let tail = parseInt(req.query.tail, 10) || 100;
    tail = Math.min(Math.max(1, tail), 500);

    const level = req.query.level;
    let logs = state.logs;

    if (level) {
      const validLevels = ['INFO', 'DEBUG', 'WARN', 'ERROR'];
      if (validLevels.includes(level.toUpperCase())) {
        logs = logs.filter((l) => l.level === level.toUpperCase());
      }
    }

    res.json({
      logs: logs.slice(-tail),
    });
  });

  return router;
}

module.exports = logsRoutes;
