'use strict';

// connect-src names only sources CSP can express. A host-source's wildcard
// may stand only for the whole leftmost label (`*.local`), so no source can
// name an IP range: `http://192.168.*` and its kin match nothing (headless
// Chromium logs each as "an invalid source ... It will be ignored"). What this reaches is the page's own origin ('self') and peers by
// their .local name on port 80; CSP also lets an http source match the same
// name over https on 443, and never over ws/wss. A peer entered or listed only
// by its IP, or on another port, is refused by CSP before any request leaves
// the page, so it has to be reached by its .local name. Widening to the
// `http:` scheme would reach IP peers, and would also let an injected script
// send data to any http host; that is not done here.
// docs/security.md's header table and docs/firmware-integration.md's snippet
// carry this string, and tests/security/security-headers.test.js holds both.
const CSP = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self'",
  "img-src 'self' data:",
  "connect-src 'self' http://*.local",
].join('; ');

function securityHeaders() {
  return (req, res, next) => {
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('X-Frame-Options', 'DENY');
    res.setHeader('Content-Security-Policy', CSP);
    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('Referrer-Policy', 'no-referrer');
    // SECURITY: Permissions-Policy disables browser features that are
    // unnecessary for a device dashboard and could be abused by injected
    // scripts (e.g., via XSS) to access camera, microphone, or geolocation.
    res.setHeader('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
    next();
  };
}

module.exports = securityHeaders;
