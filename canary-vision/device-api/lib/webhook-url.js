'use strict';

const MAX_WEBHOOK_URL_LENGTH = 512;

function parseIpv4(host) {
  // Only dotted-quad literals count; DNS names such as 192.168.attacker.example
  // must never satisfy the private-range checks below.
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  if (!m) return null;
  const octets = m.slice(1).map(Number);
  return octets.every((o) => o <= 255) ? octets : null;
}

function isAllowedWebhookHost(hostname) {
  const host = String(hostname || '').toLowerCase();
  if (!host) return false;

  // Explicitly do not allow loopback or link-local targets. On the device,
  // localhost is the device API itself and 169.254/fe80 can expose platform
  // metadata or service-discovery surfaces. Webhooks are intended for local
  // automation systems such as Home Assistant on the LAN.
  if (host === 'localhost' || host === '[::1]') return false;
  if (host.startsWith('[')) return false; // no IPv6 literals; fe80/ULA nuances aren't worth it
  if (/^\[fe80:/i.test(host)) return false;

  const ip = parseIpv4(host);
  if (ip) {
    const [a, b] = ip;
    if (a === 127 || (a === 169 && b === 254)) return false;
    // RFC1918 private LAN IPv4 addresses only. This keeps witness events local
    // by default and prevents an attacker with config access from turning the
    // device into an SSRF client against public Internet hosts.
    if (a === 10) return true;
    if (a === 192 && b === 168) return true;
    if (a === 172 && b >= 16 && b <= 31) return true;
    return false;
  }

  // Permit mDNS names for local automation systems such as Home Assistant.
  return host.endsWith('.local');
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
