'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const { validateWebhookUrl } = require('../../device-api/lib/webhook-url');
const { sendWebhook } = require('../../device-api/lib/webhook');

describe('webhook SSRF guard', () => {
  it('validates webhook targets before dispatch', async () => {
    for (const url of ['https://example.com/hook', 'http://127.0.0.1:1/hook', 'file:///tmp/x']) {
      await assert.rejects(sendWebhook(url, { ok: true }), /webhook_url|http or https/);
    }
  });

  it('allows only local automation destinations', () => {
    assert.equal(validateWebhookUrl('http://10.0.0.5/hook').ok, true);
    assert.equal(validateWebhookUrl('http://172.16.0.5/hook').ok, true);
    assert.equal(validateWebhookUrl('http://homeassistant.local/hook').ok, true);
    assert.equal(validateWebhookUrl('http://8.8.8.8/hook').ok, false);
    assert.equal(validateWebhookUrl('http://169.254.169.254/hook').ok, false);
  });
});
