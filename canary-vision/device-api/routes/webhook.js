'use strict';

const { Router } = require('express');

function webhookRoutes(state, webhookDispatcher) {
  const router = Router();

  router.post('/api/v1/webhook/test', (req, res) => {
    const url = state.config.integrations.webhook_url;
    if (!url) {
      return res.status(400).json({
        error: 'no_webhook_url',
        message: 'No webhook URL configured. Set webhook_url in integrations config first.',
      });
    }

    state.addLog('INFO', `Testing webhook: ${url}`);

    webhookDispatcher.test(url)
      .then((result) => {
        state.addLog('INFO', `Webhook test succeeded (${result.status})`);
        res.json({ ok: true, status: result.status });
      })
      .catch((err) => {
        state.addLog('WARN', `Webhook test failed: ${err.message}`);
        res.status(502).json({
          error: 'webhook_failed',
          message: err.message,
        });
      });
  });

  return router;
}

module.exports = webhookRoutes;
