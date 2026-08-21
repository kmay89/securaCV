'use strict';

const https = require('node:https');
const http = require('node:http');
const { URL } = require('node:url');
const { validateWebhookUrl } = require('./webhook-url');

// Tunables live on one object so the test suite can shrink the backoff and
// substitute the URL validator (loopback is rejected by the SSRF guard, so
// tests can't otherwise point sendWebhook at a local server). Production
// code never mutates this. Exposed via module.exports._test.
const TUNING = {
  timeoutMs: 5000,
  maxRetries: 2,
  retryDelayMs: 1000,
  validateUrl: validateWebhookUrl,
};

function sendWebhook(url, payload, attempt = 0) {
  return new Promise((resolve, reject) => {
    const validation = TUNING.validateUrl(url);
    if (!validation.ok) {
      return reject(new Error(validation.error));
    }
    const parsed = new URL(validation.url);

    const transport = parsed.protocol === 'https:' ? https : http;
    const body = JSON.stringify(payload);
    let handled = false;

    function retry(err) {
      if (handled) return;
      handled = true;
      if (attempt < TUNING.maxRetries) {
        setTimeout(() => {
          sendWebhook(url, payload, attempt + 1).then(resolve, reject);
        }, TUNING.retryDelayMs * (attempt + 1));
      } else {
        reject(err);
      }
    }

    const req = transport.request(parsed, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body),
        'User-Agent': 'SecuraCV-Canary/1.0',
      },
      timeout: TUNING.timeoutMs,
    }, (res) => {
      let data = '';
      let bytesRead = 0;
      const MAX_RESPONSE_BYTES = 64 * 1024;
      res.on('data', (chunk) => {
        bytesRead += chunk.length;
        if (bytesRead > MAX_RESPONSE_BYTES) {
          res.destroy();
          retry(new Error('Response size limit exceeded'));
          return;
        }
        data += chunk;
      });
      res.on('end', () => {
        if (handled) return;
        if (res.statusCode >= 200 && res.statusCode < 300) {
          handled = true;
          resolve({ status: res.statusCode, body: data });
        } else if (res.statusCode >= 500) {
          // retry() owns the `handled` flag. Setting it here first made
          // retry() bail out immediately: no retry was ever scheduled and
          // the promise never settled (webhook test requests hung).
          retry(new Error(`Webhook returned ${res.statusCode}`));
        } else {
          handled = true;
          reject(new Error(`Webhook returned ${res.statusCode}`));
        }
      });
    });

    req.on('error', (err) => { retry(err); });

    req.on('timeout', () => {
      req.destroy();
      retry(new Error('Webhook request timed out'));
    });

    req.write(body);
    req.end();
  });
}

function createWebhookDispatcher(state) {
  function dispatch(eventType, data) {
    const url = state.config.integrations.webhook_url;
    if (!url) return;

    const payload = {
      event: eventType,
      device_id: state.device.device_id,
      device_name: state.device.name,
      timestamp: new Date().toISOString(),
      data,
    };

    sendWebhook(url, payload).catch((err) => {
      state.addLog('WARN', `Webhook failed: ${err.message}`);
    });
  }

  function test(url) {
    const payload = {
      event: 'webhook_test',
      device_id: state.device.device_id,
      device_name: state.device.name,
      timestamp: new Date().toISOString(),
      data: { message: 'Webhook connectivity test from SecuraCV Canary' },
    };

    return sendWebhook(url, payload);
  }

  return { dispatch, test };
}

module.exports = { createWebhookDispatcher, sendWebhook };
// Exports for testing only — see the TUNING comment above.
module.exports._test = { TUNING };
