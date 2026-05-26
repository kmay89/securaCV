'use strict';

const https = require('node:https');
const http = require('node:http');
const { URL } = require('node:url');

const TIMEOUT_MS = 5000;
const MAX_RETRIES = 2;
const RETRY_DELAY_MS = 1000;

function sendWebhook(url, payload, attempt = 0) {
  return new Promise((resolve, reject) => {
    let parsed;
    try {
      parsed = new URL(url);
    } catch {
      return reject(new Error('Invalid webhook URL'));
    }

    const transport = parsed.protocol === 'https:' ? https : http;
    const body = JSON.stringify(payload);
    let handled = false;

    function retry(err) {
      if (handled) return;
      handled = true;
      if (attempt < MAX_RETRIES) {
        setTimeout(() => {
          sendWebhook(url, payload, attempt + 1).then(resolve, reject);
        }, RETRY_DELAY_MS * (attempt + 1));
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
      timeout: TIMEOUT_MS,
    }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        if (handled) return;
        handled = true;
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve({ status: res.statusCode, body: data });
        } else if (res.statusCode >= 500) {
          retry(new Error(`Webhook returned ${res.statusCode}`));
        } else {
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
