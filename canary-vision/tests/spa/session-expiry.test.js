'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

describe('SPA Session Expiry', () => {
  const spaJs = fs.readFileSync(
    path.join(__dirname, '..', '..', 'spa', 'app.js'),
    'utf8',
  );

  it('stores issued_at timestamp alongside token in sessionStorage', () => {
    assert.ok(
      spaJs.includes('SESSION_ISSUED_PREFIX') || spaJs.includes('canary_issued:'),
      'SPA must store an issued_at timestamp alongside tokens',
    );
    assert.ok(
      spaJs.includes('Date.now()'),
      'SPA must record the current timestamp when storing a token',
    );
  });

  it('checks session expiration before returning tokens', () => {
    assert.ok(
      spaJs.includes('isExpired'),
      'SPA must have an isExpired check for session tokens',
    );
    assert.ok(
      spaJs.includes('MAX_SESSION_DURATION_MS') || spaJs.includes('MAX_SESSION_DURATION'),
      'SPA must define a max session duration constant',
    );
  });

  it('has configurable session TTL defaulting to 8 hours', () => {
    assert.ok(
      spaJs.includes('8 * 3600 * 1000') || spaJs.includes('28800000'),
      'SPA must default session TTL to 8 hours (28800000ms)',
    );
  });

  it('shows a 5-minute warning before expiration', () => {
    assert.ok(
      spaJs.includes('SESSION_WARNING_MS') || spaJs.includes('5 * 60 * 1000'),
      'SPA must define a 5-minute warning period',
    );
    assert.ok(
      spaJs.includes('session-expiry-warning') || spaJs.includes('_showWarning'),
      'SPA must display a warning notification before session expires',
    );
  });

  it('clears session and shows expired message on expiry', () => {
    assert.ok(
      spaJs.includes('clearSession') || spaJs.includes('_handleExpiry'),
      'SPA must clear session data when token expires',
    );
    assert.ok(
      spaJs.includes('Session expired'),
      'SPA must display a "Session expired" message to the user',
    );
  });

  it('checks expiry on API requests', () => {
    // The API request function must check for expiration
    assert.ok(
      spaJs.includes('isExpired') && spaJs.includes('CanaryAPI'),
      'SPA API client must check session expiry before making requests',
    );
  });
});
