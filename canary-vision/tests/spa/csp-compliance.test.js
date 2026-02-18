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
