'use strict';

const { describe, it, before, after, beforeEach } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');

const webhookModule = require('../../device-api/lib/webhook');
const { sendWebhook } = webhookModule;
const { TUNING } = webhookModule._test;

// Fail loudly instead of hanging forever if the retry state machine
// regresses into leaving the promise unsettled (the original 5xx bug).
function withSettleGuard(promise, ms) {
  let timer;
  const guard = new Promise((_, reject) => {
    timer = setTimeout(() => {
      reject(new Error(`sendWebhook promise did not settle within ${ms}ms`));
    }, ms);
  });
  return Promise.race([promise, guard]).finally(() => clearTimeout(timer));
}

describe('webhook 5xx retry state machine', () => {
  let server;
  let hookUrl;
  let requestCount;
  let responsePlan; // status codes per request; the last entry repeats

  const realValidate = TUNING.validateUrl;
  const realDelay = TUNING.retryDelayMs;

  before(async () => {
    server = http.createServer((req, res) => {
      const status = responsePlan[Math.min(requestCount, responsePlan.length - 1)];
      requestCount++;
      res.statusCode = status;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ received: true }));
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    hookUrl = `http://127.0.0.1:${server.address().port}/hook`;
  });

  after(async () => {
    TUNING.validateUrl = realValidate;
    TUNING.retryDelayMs = realDelay;
    await new Promise((resolve) => server.close(resolve));
  });

  beforeEach(() => {
    requestCount = 0;
    // The SSRF guard rejects loopback by design, so tests substitute the
    // validator to reach the local server, and shrink the backoff delay
    // (production: retryDelayMs * (attempt + 1)) to keep the suite fast.
    TUNING.validateUrl = (url) => ({ ok: true, url });
    TUNING.retryDelayMs = 20;
  });

  it('retries a persistent 5xx and rejects after max retries (promise settles)', async () => {
    responsePlan = [500];
    await assert.rejects(
      withSettleGuard(sendWebhook(hookUrl, { test: 1 }), 5000),
      /Webhook returned 500/,
    );
    // Initial attempt + maxRetries retries, each hitting the target.
    assert.equal(requestCount, TUNING.maxRetries + 1);
  });

  it('resolves when a retry after a 5xx succeeds', async () => {
    responsePlan = [502, 200];
    const result = await withSettleGuard(sendWebhook(hookUrl, { test: 2 }), 5000);
    assert.equal(result.status, 200);
    assert.equal(requestCount, 2);
  });

  it('does not retry 4xx responses', async () => {
    responsePlan = [404];
    await assert.rejects(
      withSettleGuard(sendWebhook(hookUrl, { test: 3 }), 5000),
      /Webhook returned 404/,
    );
    assert.equal(requestCount, 1);
  });

  it('keeps the SSRF guard active by default', async () => {
    TUNING.validateUrl = realValidate;
    responsePlan = [200];
    await assert.rejects(
      withSettleGuard(sendWebhook(hookUrl, { test: 4 }), 5000),
      /webhook_url/,
    );
    assert.equal(requestCount, 0);
  });
});
