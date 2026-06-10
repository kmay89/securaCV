'use strict';

// Server-side mirror of the SPA's isPrivateUrl(): only RFC 1918,
// link-local-ish, loopback, and .local (mDNS) origins qualify. Used to
// decide which origins may ever be enrolled by trust-on-pair.
function isPrivateOrigin(origin) {
  let url;
  try {
    url = new URL(origin);
  } catch {
    return false;
  }
  if (url.protocol !== 'http:' && url.protocol !== 'https:') return false;
  const hostname = url.hostname;
  if (hostname.endsWith('.local')) return true;
  if (/^192\.168\./.test(hostname)) return true;
  if (/^10\./.test(hostname)) return true;
  if (/^172\.(1[6-9]|2[0-9]|3[01])\./.test(hostname)) return true;
  if (/^169\.254\./.test(hostname)) return true; // IPv4 link-local
  if (hostname === 'localhost' || hostname === '127.0.0.1') return true;
  // IPv6: URL.hostname keeps the brackets. Loopback, link-local (fe80::/10),
  // and Unique Local Addresses (fc00::/7) are all private-network scopes.
  if (hostname === '[::1]') return true;
  if (/^\[fe80:/i.test(hostname)) return true;
  if (/^\[f[cd]/i.test(hostname)) return true;
  return false;
}

module.exports = { isPrivateOrigin };
