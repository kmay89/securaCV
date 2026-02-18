'use strict';

function rateLimit(options = {}) {
  const generalLimit = options.generalLimit || 30;
  const generalWindowMs = options.generalWindowMs || 60000;
  const authFailLimit = options.authFailLimit || 5;
  const authLockoutMs = options.authLockoutMs || 60000;
  const maxEntries = options.maxEntries || 64;

  // Map<ip, { requests: [{ts}], authFailures: [{ts}], lockedUntil: number }>
  const ipMap = new Map();

  function getEntry(ip) {
    if (!ipMap.has(ip)) {
      if (ipMap.size >= maxEntries) {
        // LRU eviction: remove the oldest-accessed entry
        const oldestKey = ipMap.keys().next().value;
        ipMap.delete(oldestKey);
      }
      ipMap.set(ip, { requests: [], authFailures: [], lockedUntil: 0 });
    }
    // Move to end (most recently used)
    const entry = ipMap.get(ip);
    ipMap.delete(ip);
    ipMap.set(ip, entry);
    return entry;
  }

  function cleanup(arr, windowMs) {
    const cutoff = Date.now() - windowMs;
    while (arr.length > 0 && arr[0] < cutoff) {
      arr.shift();
    }
  }

  function middleware(req, res, next) {
    const ip = req.ip || req.socket.remoteAddress;
    const entry = getEntry(ip);
    const now = Date.now();

    // Check auth lockout
    if (entry.lockedUntil > now) {
      const retryAfter = Math.ceil((entry.lockedUntil - now) / 1000);
      res.setHeader('Retry-After', String(retryAfter));
      return res.status(429).json({
        error: 'auth_locked',
        message: 'Too many authentication failures. Locked for 60 seconds.',
        retry_after: retryAfter,
      });
    }

    // Clean up expired entries
    cleanup(entry.requests, generalWindowMs);
    cleanup(entry.authFailures, authLockoutMs);

    // Check general rate limit
    if (entry.requests.length >= generalLimit) {
      const oldestInWindow = entry.requests[0];
      const retryAfter = Math.ceil((oldestInWindow + generalWindowMs - now) / 1000);
      res.setHeader('Retry-After', String(retryAfter));
      return res.status(429).json({
        error: 'rate_limited',
        message: 'Too many requests. Try again in 60 seconds.',
        retry_after: retryAfter,
      });
    }

    // Track request
    entry.requests.push(now);

    // Expose a method to record auth failures
    res.recordAuthFailure = () => {
      entry.authFailures.push(Date.now());
      if (entry.authFailures.length >= authFailLimit) {
        entry.lockedUntil = Date.now() + authLockoutMs;
      }
    };

    next();
  }

  // Expose for testing
  middleware._ipMap = ipMap;
  middleware._reset = () => ipMap.clear();

  return middleware;
}

module.exports = rateLimit;
