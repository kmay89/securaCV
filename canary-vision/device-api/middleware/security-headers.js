'use strict';

const CSP = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self'",
  "img-src 'self' data:",
  "connect-src 'self' http://*.local http://192.168.* http://10.* http://172.16.* http://172.17.* http://172.18.* http://172.19.* http://172.20.* http://172.21.* http://172.22.* http://172.23.* http://172.24.* http://172.25.* http://172.26.* http://172.27.* http://172.28.* http://172.29.* http://172.30.* http://172.31.*",
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
