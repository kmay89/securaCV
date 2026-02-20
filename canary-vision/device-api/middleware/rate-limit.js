'use strict';

function rateLimit(options = {}) {
  const generalLimit = options.generalLimit || 30;
  const generalWindowMs = options.generalWindowMs || 60000;
  const authFailLimit = options.authFailLimit || 5;
  // Exponential backoff aligned with firmware's DEFAULT_AUTH_LOCKOUT_BASE_SEC=2
  // and DEFAULT_AUTH_LOCKOUT_CAP_SEC=300.
  const authLockoutBaseMs = options.authLockoutBaseMs || 2000;
  const authLockoutCapMs = options.authLockoutCapMs || 300000;
  const maxEntries = options.maxEntries || 64;

  // Map<ip, { requests: [{ts}], authFailures: [{ts}], lockedUntil: number, lockoutCount: number }>
  const ipMap = new Map();

  function getEntry(ip) {
    if (!ipMap.has(ip)) {
      if (ipMap.size >= maxEntries) {
        // LRU eviction: remove the oldest-accessed entry
        const oldestKey = ipMap.keys().next().value;
        ipMap.delete(oldestKey);
      }
      ipMap.set(ip, { requests: [], authFailures: [], lockedUntil: 0, lockoutCount: 0 });
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
        message: `Too many authentication failures. Locked for ${retryAfter} seconds.`,
        retry_after: retryAfter,
      });
    }

    // Clean up expired entries
    cleanup(entry.requests, generalWindowMs);
    // Use cap window for auth failure cleanup so lockout history persists
    cleanup(entry.authFailures, authLockoutCapMs);

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

    // Expose a method to record auth failures with exponential backoff.
    // Firmware pattern: DEFAULT_AUTH_LOCKOUT_BASE_SEC * 2^(lockoutCount)
    // capped at DEFAULT_AUTH_LOCKOUT_CAP_SEC.
    res.recordAuthFailure = () => {
      entry.authFailures.push(Date.now());
      if (entry.authFailures.length >= authFailLimit) {
        entry.lockoutCount++;
        const lockoutMs = Math.min(
          authLockoutBaseMs * Math.pow(2, entry.lockoutCount - 1),
          authLockoutCapMs
        );
        entry.lockedUntil = Date.now() + lockoutMs;
        // Clear failure count to start fresh after lockout
        entry.authFailures = [];
      }
    };

    // Record successful auth — resets lockout escalation so a previously
    // locked-out user starts fresh after a good login.
    res.recordAuthSuccess = () => {
      entry.lockoutCount = 0;
      entry.authFailures = [];
      entry.lockedUntil = 0;
    };

    next();
  }

  // Expose for testing
  middleware._ipMap = ipMap;
  middleware._reset = () => ipMap.clear();

  return middleware;
}

module.exports = rateLimit;
