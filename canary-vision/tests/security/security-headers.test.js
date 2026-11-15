'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;
const DOCS_DIR = path.join(__dirname, '..', '..', 'docs');

// CSP Level 3 §2.3.1 source-expression grammar, one alternative per regex.
// A host-source's wildcard may be only the whole host ("*") or the whole
// leftmost label ("*." before the rest), so a source such as
// `http://192.168.*` is not a host-source at all: it matches nothing, and
// headless Chromium logs "contains an invalid source ... It will be ignored".
// No source can name an IP range.
const SCHEME = '[A-Za-z][A-Za-z0-9+.-]*';
const HOST_CHARS = '[A-Za-z0-9-]+';
const HOST_PART = `(?:\\*|(?:\\*\\.)?${HOST_CHARS}(?:\\.${HOST_CHARS})*\\.?)`;
const PORT_PART = '(?:[0-9]+|\\*)';
// path-absolute: "/" optionally followed by a non-empty segment, no "//";
// a source expression holds no whitespace, ";" or ",".
const PATH_PART = '(?:/(?:[^/\\s;,][^\\s;,]*)?)';
const SOURCE_FORMS = {
  keyword: /^'(?:self|unsafe-inline|unsafe-eval|strict-dynamic|unsafe-hashes|report-sample|unsafe-allow-redirects|wasm-unsafe-eval)'$/i,
  nonce: /^'nonce-[A-Za-z0-9+/_-]+={0,2}'$/,
  hash: /^'sha(?:256|384|512)-[A-Za-z0-9+/_-]+={0,2}'$/i,
  scheme: new RegExp(`^${SCHEME}:$`),
  host: new RegExp(`^(?:${SCHEME}://)?${HOST_PART}(?::${PORT_PART})?${PATH_PART}?$`),
};

function sourceForm(expr) {
  for (const [form, re] of Object.entries(SOURCE_FORMS)) {
    if (re.test(expr)) return form;
  }
  return null;
}

function directive(csp, name) {
  for (const part of csp.split(';')) {
    const tokens = part.trim().split(/\s+/);
    if (tokens[0].toLowerCase() === name) return tokens.slice(1);
  }
  return null;
}

describe('Security Headers', () => {
  let server, client;

  before(async () => {
    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
  });

  after(async () => {
    await server.close();
  });

  function assertSecurityHeaders(headers) {
    assert.equal(headers['x-content-type-options'], 'nosniff');
    assert.equal(headers['x-frame-options'], 'DENY');
    assert.ok(headers['content-security-policy'].startsWith("default-src 'self'"));
    assert.equal(headers['cache-control'], 'no-store');
    assert.equal(headers['referrer-policy'], 'no-referrer');
  }

  it('Security headers present on API responses', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.status, 200);
    assertSecurityHeaders(res.headers);
  });

  it('Security headers present on static file responses', async () => {
    const res = await request(server.url + '/', {
      headers: { Host: '127.0.0.1' },
    });
    assert.equal(res.status, 200);
    assertSecurityHeaders(res.headers);
  });

  it('X-Frame-Options DENY prevents iframe embedding', async () => {
    const res = await client.get('/api/v1/info');
    assert.equal(res.headers['x-frame-options'], 'DENY',
      'X-Frame-Options must be exactly DENY, not SAMEORIGIN');
  });

  it('every connect-src source is one CSP can express', async () => {
    const res = await client.get('/api/v1/info');
    const sources = directive(res.headers['content-security-policy'], 'connect-src');
    assert.ok(sources && sources.length > 0, 'connect-src must be present');
    const invalid = sources.filter((s) => sourceForm(s) === null);
    assert.deepEqual(invalid, [],
      'connect-src names sources outside the CSP3 grammar (a wildcard may be '
      + 'only the whole leftmost label, so no source names an IP range); '
      + 'headless Chromium ignores each: ' + invalid.join(' '));
  });

  it('connect-src reaches its own origin and peers by .local name', async () => {
    const res = await client.get('/api/v1/info');
    const sources = directive(res.headers['content-security-policy'], 'connect-src');
    assert.ok(sources.includes("'self'"), "connect-src must keep 'self'");
    assert.ok(sources.includes('http://*.local'), 'connect-src must keep http://*.local');
  });

  it('docs/security.md states the served Content-Security-Policy', async () => {
    const res = await client.get('/api/v1/info');
    const md = fs.readFileSync(path.join(DOCS_DIR, 'security.md'), 'utf8');
    const row = /^\| `Content-Security-Policy` \| `([^`]*)` \|$/m.exec(md);
    assert.ok(row, 'security.md has a Content-Security-Policy row in its header table');
    assert.equal(row[1], res.headers['content-security-policy']);
  });

  it('docs/firmware-integration.md tells firmware to serve the same policy', async () => {
    const res = await client.get('/api/v1/info');
    const md = fs.readFileSync(path.join(DOCS_DIR, 'firmware-integration.md'), 'utf8');
    const call = /httpd_resp_set_hdr\(req, "Content-Security-Policy",([\s\S]*?)\);/.exec(md);
    assert.ok(call, 'firmware-integration.md has the Content-Security-Policy call');
    const literals = [...call[1].matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1]);
    assert.equal(literals.join(''), res.headers['content-security-policy']);
  });
});

describe('CSP source-expression grammar (the check above)', () => {
  it('accepts the forms CSP3 defines', () => {
    for (const ok of ["'self'", "'unsafe-inline'", 'http:', 'https:',
      'http://*.local', '*.local', 'canary-a3f7.local', 'http://canary-a3f7.local:80',
      'http://192.168.1.47', 'https://*.example.com:*/api/', '*', 'http://*',
      "'nonce-abc123=='", "'sha256-AbC+/xyz='"]) {
      assert.notEqual(sourceForm(ok), null, ok + ' is a CSP3 source expression');
    }
  });

  it('refuses a wildcard that is not the whole leftmost label', () => {
    for (const bad of ['http://192.168.*', 'http://10.*', 'http://172.16.*',
      'http://172.16-31.*', 'http://a.*.local', 'http://*canary.local',
      'http://canary*.local', '*.*.local', 'http://**.local', 'http://*.', 'http://1.2.3.*']) {
      assert.equal(sourceForm(bad), null, bad + ' is not a CSP3 source expression');
    }
  });
});
