'use strict';

const http = require('node:http');

/**
 * Minimal HTTP client for testing.
 * Does NOT rely on fetch so we can control Host headers etc.
 */
function request(url, options = {}) {
  return new Promise((resolve, reject) => {
    const parsed = new URL(url);
    const reqOptions = {
      hostname: parsed.hostname,
      port: parsed.port,
      path: parsed.pathname + parsed.search,
      method: options.method || 'GET',
      headers: { ...options.headers },
    };

    // Set Host header if provided (overriding default)
    if (options.host) {
      reqOptions.headers['Host'] = options.host;
    }

    const req = http.request(reqOptions, (res) => {
      let body = '';
      res.on('data', (chunk) => { body += chunk; });
      res.on('end', () => {
        let json = null;
        try { json = JSON.parse(body); } catch (e) { /* not json */ }
        resolve({
          status: res.statusCode,
          headers: res.headers,
          body,
          json,
        });
      });
    });

    req.on('error', reject);

    if (options.body) {
      const data = typeof options.body === 'string' ? options.body : JSON.stringify(options.body);
      if (!reqOptions.headers['Content-Type'] && typeof options.body !== 'string') {
        reqOptions.headers['Content-Type'] = 'application/json';
      }
      req.write(data);
    }

    req.end();
  });
}

/**
 * Creates a client pre-configured with a server URL and token.
 */
function createClient(baseUrl, token) {
  return {
    get(path, opts = {}) {
      return request(baseUrl + path, {
        method: 'GET',
        headers: {
          'X-Canary-Token': token,
          'Host': opts.host || '127.0.0.1',
          ...opts.headers,
        },
        host: opts.host,
      });
    },
    put(path, body, opts = {}) {
      return request(baseUrl + path, {
        method: 'PUT',
        headers: {
          'X-Canary-Token': token,
          'Content-Type': 'application/json',
          'Host': opts.host || '127.0.0.1',
          ...opts.headers,
        },
        body: JSON.stringify(body),
        host: opts.host,
      });
    },
    post(path, body, opts = {}) {
      return request(baseUrl + path, {
        method: 'POST',
        headers: {
          'X-Canary-Token': token,
          'Content-Type': 'application/json',
          'Host': opts.host || '127.0.0.1',
          ...opts.headers,
        },
        body: body ? JSON.stringify(body) : undefined,
        host: opts.host,
      });
    },
    options(path, opts = {}) {
      return request(baseUrl + path, {
        method: 'OPTIONS',
        headers: {
          'Host': opts.host || '127.0.0.1',
          ...opts.headers,
        },
        host: opts.host,
      });
    },
    raw(path, opts = {}) {
      return request(baseUrl + path, opts);
    },
  };
}

module.exports = { request, createClient };
