'use strict';

const MAX_WEBHOOK_URL_LENGTH = 512;

function isAllowedWebhookHost(hostname) {
  const host = String(hostname || '').toLowerCase();
  if (!host) return false;

  // Explicitly do not allow loopback or link-local targets. On the device,
  // localhost is the device API itself and 169.254/fe80 can expose platform
  // metadata or service-discovery surfaces. Webhooks are intended for local
  // automation systems such as Home Assistant on the LAN.
  if (host === 'localhost' || host === '127.0.0.1' || host === '[::1]') return false;
  if (/^127\./.test(host)) return false;
  if (/^169\.254\./.test(host)) return false;
  if (/^\[fe80:/i.test(host)) return false;

  // Permit mDNS names and RFC1918/private LAN IPv4 addresses only. This keeps
  // witness events local by default and prevents an attacker with config access
  // from turning the device into an SSRF client against public Internet hosts.
  if (host.endsWith('.local')) return true;
  if (/^192\.168\./.test(host)) return true;
  if (/^10\./.test(host)) return true;
  if (/^172\.(1[6-9]|2[0-9]|3[01])\./.test(host)) return true;
  return false;
}

function validateWebhookUrl(value) {
  if (value === '') return { ok: true, url: '' };
  if (typeof value !== 'string') {
    return { ok: false, error: 'webhook_url must be a string' };
  }
  if (value.length > MAX_WEBHOOK_URL_LENGTH) {
    return { ok: false, error: `webhook_url must be at most ${MAX_WEBHOOK_URL_LENGTH} characters` };
  }

  let parsed;
  try {
    parsed = new URL(value);
  } catch {
    return { ok: false, error: 'webhook_url must be an absolute HTTP(S) URL' };
  }

  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    return { ok: false, error: 'webhook_url must use http or https' };
  }
  if (parsed.username || parsed.password) {
    return { ok: false, error: 'webhook_url must not contain username or password' };
  }
  if (!isAllowedWebhookHost(parsed.hostname)) {
    return { ok: false, error: 'webhook_url host must be an RFC1918 LAN address or .local name; loopback, link-local, and public hosts are not allowed' };
  }

  parsed.hash = '';
  return { ok: true, url: parsed.toString() };
}

module.exports = { validateWebhookUrl, isAllowedWebhookHost, MAX_WEBHOOK_URL_LENGTH };
