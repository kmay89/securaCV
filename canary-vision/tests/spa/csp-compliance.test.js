'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const SPA_DIR = path.join(__dirname, '..', '..', 'spa');

describe('SPA CSP Compliance', () => {
  it('SPA has no inline scripts', () => {
    const html = fs.readFileSync(path.join(SPA_DIR, 'index.html'), 'utf8');

    // Check for script tags with inline content
    const scriptRegex = /<script(?![^>]*\bsrc\b)[^>]*>[\s\S]*?\S[\s\S]*?<\/script>/gi;
    const inlineScripts = html.match(scriptRegex);
    assert.equal(inlineScripts, null, 'No inline scripts should exist in index.html');

    // Check for on* event handler attributes
    const eventRegex = /\bon\w+\s*=/gi;
    const eventHandlers = html.match(eventRegex);
    assert.equal(eventHandlers, null, 'No on* event handlers should exist in index.html');
  });

  it('SPA does not use eval or Function constructor', () => {
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    assert.ok(!js.includes('eval('), 'app.js must not contain eval()');
    assert.ok(!js.includes('new Function('), 'app.js must not contain new Function()');
  });

  it('SPA does not use document.write', () => {
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    assert.ok(!js.includes('document.write'), 'app.js must not contain document.write');
  });

  it('SPA does not use innerHTML with dynamic content', () => {
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    assert.ok(!js.includes('.innerHTML'), 'app.js must not use .innerHTML');
  });

  // app.js refuses an IP that is not its own origin before fetching it
  // (needsLocalName(), sweep F448) because the CSP served with it has
  // connect-src 'self' http://*.local, which the browser would refuse it
  // under anyway. These rows hold the pieces that claim rests on: the served
  // connect-src is exactly that pair (widen it and an IP the page could then
  // reach would be refused by the SPA for nothing, so revisit
  // needsLocalName() with it), every request the page makes goes through
  // CanaryAPI.request(), where the refusal sits, and no address is built from
  // an `ip` field or suggested as an IP. tests/spa/peer-discovery.test.js
  // runs the paths.
  it("needsLocalName() is written for the served connect-src 'self' http://*.local", () => {
    const securityHeaders = require('../../device-api/middleware/security-headers');
    const headers = {};
    securityHeaders()({}, { setHeader: (k, v) => { headers[k.toLowerCase()] = v; } }, () => {});
    const connect = headers['content-security-policy'].split(';').map((d) => d.trim().split(/\s+/))
      .find((d) => d[0] === 'connect-src');
    assert.deepEqual(connect.slice(1), ["'self'", 'http://*.local'],
      'connect-src changed: needsLocalName() in spa/app.js refuses what this policy refuses; update both together');
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    assert.match(js, /function needsLocalName\(url\)/);
  });

  it('every request app.js makes goes through CanaryAPI.request()', () => {
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    const fetches = js.match(/\bfetch\(/g) || [];
    assert.equal(fetches.length, 1, 'app.js calls fetch() outside CanaryAPI.request()');
    const request = js.slice(js.indexOf('  request: function (baseUrl, path, options) {'));
    assert.ok(request.indexOf('needsLocalName(url)') >= 0
      && request.indexOf('needsLocalName(url)') < request.indexOf('fetch(url'),
      'CanaryAPI.request() must refuse an address the served CSP refuses before fetch()');
    for (const api of ['XMLHttpRequest', 'WebSocket', 'sendBeacon']) {
      assert.ok(!js.includes(api), 'app.js uses ' + api + ', which bypasses CanaryAPI.request()');
    }
    // The one EventSource opens on a ticket that CanaryAPI.request() fetched
    // from the same base_url first.
    assert.equal((js.match(/new EventSource\(/g) || []).length, 1);
    const stream = js.slice(js.indexOf('function connectDeviceStream('), js.indexOf('new EventSource('));
    assert.match(stream, /CanaryAPI\.request\(device\.base_url, '\/api\/v1\/witness\/stream\/ticket'/);
  });

  it('app.js builds no address from an ip field and suggests no IP', () => {
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    assert.deepEqual(js.match(/[\w\]]\.ip\b/g), null,
      "app.js reads an `ip` field: an IP that is not this page's origin is refused by the served CSP");
    const placeholders = [...js.matchAll(/placeholder: '([^']*)'/g)].map((m) => m[1]);
    assert.ok(placeholders.length > 0);
    assert.deepEqual(placeholders.filter((p) => /\b\d{1,3}(?:\.\d{1,3}){3}\b|\[[0-9a-f:]+\]/i.test(p)), [],
      'a placeholder suggests an IP address');
  });

  it('SPA does not include tracking or analytics', () => {
    const html = fs.readFileSync(path.join(SPA_DIR, 'index.html'), 'utf8');
    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    const combined = html + js;

    const trackers = [
      'google-analytics', 'gtag', 'mixpanel', 'segment',
      'amplitude', 'hotjar', 'plausible', 'umami',
    ];

    for (const tracker of trackers) {
      assert.ok(!combined.toLowerCase().includes(tracker),
        `SPA must not reference tracking service: ${tracker}`);
    }
  });
});
