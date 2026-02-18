'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

describe('SPA Routing', () => {
  it('SPA defines only permitted routes', () => {
    const js = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');

    // Extract route registrations
    const routeRegex = /Router\.register\s*\(\s*['"]([^'"]+)['"]/g;
    const routes = [];
    let match;
    while ((match = routeRegex.exec(js)) !== null) {
      routes.push(match[1]);
    }

    assert.ok(routes.length > 0, 'Should have registered routes');

    // Permitted routes
    const permitted = [
      '/canaries',
      '/canaries/add',
      '/device/:id',
      '/device/:id/config/:section',
      '/device/:id/logs',
      '/events',
      '/settings',
    ];

    for (const route of routes) {
      assert.ok(permitted.includes(route),
        `Route "${route}" is not in the permitted list`);
    }

    // Forbidden routes must NOT exist
    const forbidden = ['/scan', '/discover', '/fleet', '/admin'];
    for (const route of routes) {
      for (const f of forbidden) {
        assert.ok(!route.includes(f),
          `Route "${route}" contains forbidden pattern "${f}"`);
      }
    }
  });
});
