'use strict';

// Unit tests for the SPA's device-type awareness. app.js is browser-only with no module
// exports, so we load it in a VM sandbox with minimal browser-global stubs (the same
// approach as verified-timeline.test.js) and exercise the pure logic directly: the
// DEVICE_TYPES registry, dt canonicalization, the wizard branch decision, and
// device_type persistence through the CanaryStorage allowlist.

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// In-memory Storage stand-in so CanaryStorage round-trips actually run.
function memStorage() {
  const store = new Map();
  return {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => { store.set(k, String(v)); },
    removeItem: (k) => { store.delete(k); },
    _raw: (k) => store.get(k),
  };
}

function loadApp() {
  const code = fs.readFileSync(path.join(__dirname, '..', '..', 'spa', 'app.js'), 'utf8');
  const noop = () => {};
  const sandbox = {
    document: { addEventListener: noop, getElementById: () => null, createElement: () => ({}) },
    window: { addEventListener: noop, location: { hash: '' }, history: {}, matchMedia: () => ({ matches: false, addEventListener: noop }) },
    localStorage: memStorage(),
    sessionStorage: memStorage(),
    navigator: { onLine: true },
    setInterval: noop, clearInterval: noop, setTimeout: noop, clearTimeout: noop,
    fetch: noop, console,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(code, sandbox);
  return sandbox;
}

describe('SPA device types', () => {
  let app;
  before(() => { app = loadApp(); });

  it('registry covers the three witness variants plus an unknown fallback', () => {
    ['canary-wap', 'canary-vision', 'canary-sense', 'unknown'].forEach((dt) => {
      const info = app.DEVICE_TYPES[dt];
      assert.ok(info, `missing DEVICE_TYPES entry: ${dt}`);
      assert.equal(typeof info.label, 'string');
      assert.equal(typeof info.tagline, 'string');
      assert.ok(Array.isArray(info.whatsDifferent));
    });
    assert.equal(app.DEVICE_TYPES['canary-wap'].pairing, 'http');
    assert.equal(app.DEVICE_TYPES['canary-vision'].pairing, 'mqtt');
    assert.equal(app.DEVICE_TYPES['canary-sense'].pairing, 'mqtt');
    assert.equal(app.DEVICE_TYPES.unknown.pairing, 'http');
  });

  it('canonicalDeviceType folds any spelling onto the hyphenated key', () => {
    assert.equal(app.canonicalDeviceType('canary_sense'), 'canary-sense');
    assert.equal(app.canonicalDeviceType('Canary Vision'), 'canary-vision');
    assert.equal(app.canonicalDeviceType('  CANARY-WAP  '), 'canary-wap');
    assert.equal(app.canonicalDeviceType(''), '');
    assert.equal(app.canonicalDeviceType(null), '');
    assert.equal(app.canonicalDeviceType(42), '');
  });

  it('deviceTypeInfo falls back to the unknown entry', () => {
    assert.equal(app.deviceTypeInfo('canary-vision').label, 'Canary Vision');
    assert.equal(app.deviceTypeInfo('canary-mystery'), app.DEVICE_TYPES.unknown);
    assert.equal(app.deviceTypeInfo(''), app.DEVICE_TYPES.unknown);
  });

  it('wizardPathForPeer routes vision/sense to MQTT and everything else to HTTP', () => {
    // Peers relay the mDNS TXT `dt` key…
    assert.equal(app.wizardPathForPeer({ dt: 'canary-vision' }), 'mqtt');
    assert.equal(app.wizardPathForPeer({ dt: 'canary-sense' }), 'mqtt');
    assert.equal(app.wizardPathForPeer({ dt: 'canary-wap' }), 'http');
    // …but the long device_type spelling (and sloppy casing) must work too.
    assert.equal(app.wizardPathForPeer({ device_type: 'canary_sense' }), 'mqtt');
    assert.equal(app.wizardPathForPeer({ device_type: 'Canary-Vision' }), 'mqtt');
    // Untyped peers keep today's HTTP BOOT-tap flow.
    assert.equal(app.wizardPathForPeer({ device_id: 'canary-old1' }), 'http');
    assert.equal(app.wizardPathForPeer(null), 'http');
  });

  it('CanaryStorage persists device_type through the save/load allowlist', () => {
    app.CanaryStorage.saveDevices([{
      id: 'canary-b1c2',
      name: 'Garage',
      base_url: 'http://canary-b1c2.local',
      room: 'Garage',
      device_type: 'canary-vision',
      token: 'cv_b1c2_secret',
      last_info: { device_id: 'canary-b1c2' },
      added_at: '2026-02-18T15:30:00.000Z',
    }]);

    const loaded = app.CanaryStorage.getDevices();
    assert.equal(loaded.length, 1);
    assert.equal(loaded[0].device_type, 'canary-vision');

    // The allowlist must not leak the token into localStorage.
    const raw = app.localStorage._raw('canary_devices');
    assert.ok(raw.includes('"device_type":"canary-vision"'));
    assert.ok(!raw.includes('cv_b1c2_secret'));
  });
});

// The canary-wap card must not say this SPA pairs a WAP (sweep F355). The
// canary-wap firmware does not answer the SPA's pairing: its receipt route
// sends no Access-Control-Allow-Origin, so a browser hides its 403, its 409
// and its receipt from this page (recorded in Chromium against a stub that
// answers the way the sketch does: the capture never saw asked=true and ended
// onTimeout(false)), it reads `Authorization: Bearer` and never
// `X-Canary-Token`, and it registers no /api/v1/info or /api/v1/peers. The
// card is held to those facts, read from the sketch: if a WAP ever answers
// this SPA's pairing, the premise row fails and names what to revisit. The
// card renders on the success screen, under "Canary WAP paired" (only the
// reference device-api, which reports itself as a canary-wap, gets there), so
// it says where a WAP pairs without denying that screen's pairing
// (receipt-capture.test.js holds the screen).
const WAP_SKETCH_DIR = path.join(__dirname, '..', '..', '..',
  'firmware', 'projects', 'canary-wap', 'arduino', 'canary_wap');

// Every card line that says or implies a WAP pairs in this SPA, or that
// mentions this app at all (the card sits on this app's success screen).
function spaPairingClaims(lines) {
  return lines.filter((line) => /\bhere\b/i.test(line)
    || /\bover HTTP\b/i.test(line)
    || /\bBOOT\b/.test(line)
    || /\bthis (?:app|page|SPA)\b/i.test(line)
    || /\bin the app\b/i.test(line)
    || /\bone (?:press|tap)\b/i.test(line)
    || /\bdashboard QR\b|\bShow QR\b|\bpairing QR\b/i.test(line));
}

// C source with comments blanked (string and char literals kept whole, so a
// brace or "//" inside one is not code).
function stripComments(src) {
  let out = '';
  for (let i = 0; i < src.length;) {
    const c = src[i];
    const n = src[i + 1];
    if (c === '/' && n === '/') {
      const j = src.indexOf('\n', i);
      const end = j < 0 ? src.length : j;
      out += ' '.repeat(end - i);
      i = end;
    } else if (c === '/' && n === '*') {
      const j = src.indexOf('*/', i + 2);
      const end = j < 0 ? src.length : j + 2;
      out += src.slice(i, end).replace(/[^\n]/g, ' ');
      i = end;
    } else if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < src.length && src[j] !== c && src[j] !== '\n') j += src[j] === '\\' ? 2 : 1;
      out += src.slice(i, j + 1);
      i = j + 1;
    } else {
      out += c;
      i += 1;
    }
  }
  return out;
}

// name -> body text of every function defined in `src` (comment-stripped),
// braces matched outside string and char literals.
function functionBodies(src) {
  const code = stripComments(src);
  const bodies = new Map();
  // A definition starts at column 0 with its return type on the same line.
  const def = /^[A-Za-z_][\w:<>*& \t]*?\b([A-Za-z_]\w*)[ \t]*\([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*\)\s*(?:const\s*)?\{/gm;
  let m;
  while ((m = def.exec(code))) {
    if (/^(?:if|for|while|switch|return|else|do|sizeof)$/.test(m[1])) continue;
    let depth = 0;
    let i = m.index + m[0].length - 1;
    for (; i < code.length; i++) {
      const c = code[i];
      if (c === '"' || c === "'") {
        let j = i + 1;
        while (j < code.length && code[j] !== c && code[j] !== '\n') j += code[j] === '\\' ? 2 : 1;
        i = j;
      } else if (c === '{') depth++;
      else if (c === '}' && --depth === 0) break;
    }
    if (!bodies.has(m[1])) bodies.set(m[1], code.slice(m.index, i + 1));
    def.lastIndex = i + 1;
  }
  return bodies;
}

const ACAO = 'Access-Control-Allow-Origin';

// Every call in `body`: { name, args } with args the text between the call's
// own parentheses (matched, so a nested call does not end it).
function callsIn(body) {
  const out = [];
  const open = /\b([A-Za-z_]\w*)\s*\(/g;
  let m;
  while ((m = open.exec(body))) {
    let depth = 0;
    let i = m.index + m[0].length - 1;
    for (; i < body.length; i++) {
      const c = body[i];
      if (c === '"' || c === "'") {
        let j = i + 1;
        while (j < body.length && body[j] !== c && body[j] !== '\n') j += body[j] === '\\' ? 2 : 1;
        i = j;
      } else if (c === '(') depth++;
      else if (c === ')' && --depth === 0) break;
    }
    out.push({ name: m[1], args: body.slice(m.index + m[0].length, i) });
  }
  return out;
}

// The calls in `calls` (one body's) to a CORS sender that can answer the
// pairing: every call except an error answer with a 5xx status literal
// (`http_send_error(req, 503, ...)`), which is not the 403, 409 or receipt
// the SPA's pairing reads.
function corsCalls(calls, senders, self) {
  return calls.filter((c) => c.name !== self && senders.has(c.name)
    && !/^\s*req\s*,\s*5\d\d\s*,/.test(c.args));
}

// Every way the receipt route's answers carry CORS in `sources` (the sketch's
// files, name -> text). A CORS sender is a function whose body sets the
// header, or answers through one that does (corsCalls; resolved to a
// fixpoint, so http_send_error -> http_send_json counts). In the two receipt
// functions a direct header, or a corsCalls() call, is a finding.
function receiptCorsFindings(sources) {
  const bodies = new Map();
  for (const text of sources.values()) {
    for (const [name, body] of functionBodies(text)) {
      if (!bodies.has(name)) {
        const inner = body.slice(body.indexOf('{'));
        bodies.set(name, { inner, calls: callsIn(inner) });
      }
    }
  }
  const senders = new Set([...bodies].filter(([, b]) => b.inner.includes(ACAO)).map(([n]) => n));
  for (let grew = true; grew;) {
    grew = false;
    for (const [name, b] of bodies) {
      if (!senders.has(name) && corsCalls(b.calls, senders, name).length) {
        senders.add(name);
        grew = true;
      }
    }
  }
  const findings = [];
  for (const fn of ['handle_provisioning_receipt', 'send_provisioning_receipt']) {
    const b = bodies.get(fn);
    if (!b) { findings.push(`canary_wap.ino no longer defines ${fn}()`); continue; }
    if (b.inner.includes(ACAO)) findings.push(`${fn}() sets ${ACAO} itself`);
    for (const c of corsCalls(b.calls, senders, fn)) {
      findings.push(`${fn}() answers through ${c.name}(), which sends ${ACAO}: `
        + `${c.name}(${c.args.replace(/\s+/g, ' ').trim()})`);
    }
  }
  return findings;
}

function readSketch() {
  const files = fs.readdirSync(WAP_SKETCH_DIR).filter((f) => /\.(?:ino|h|cpp)$/.test(f));
  return new Map(files.map((f) => [f, fs.readFileSync(path.join(WAP_SKETCH_DIR, f), 'utf8')]));
}

describe('SPA device types: the canary-wap card makes no SPA-pairing claim', () => {
  let app;
  let sketch;
  before(() => { app = loadApp(); sketch = readSketch(); });

  it('premise: the canary-wap firmware does not answer this SPA\'s pairing', () => {
    const all = [...sketch.values()].join('\n');
    const revisit = ' — the WAP may now answer this SPA\'s pairing: revisit '
      + "DEVICE_TYPES['canary-wap'] and docs/discovery.md §2-§4";
    assert.ok(!/\.uri\s*=\s*"\/api\/v1\/(?:info|peers)"/.test(all),
      'the sketch registers /api/v1/info or /api/v1/peers' + revisit);
    assert.ok(!/X-Canary-Token/i.test(all), 'the sketch reads X-Canary-Token' + revisit);
    assert.deepEqual(receiptCorsFindings(sketch), [],
      'the receipt route now answers with CORS' + revisit);
  });

  it('the CORS check sees a header set directly or through the sketch\'s JSON helpers', () => {
    const ino = sketch.get('canary_wap.ino');
    // Replace `from` with `to` inside function `fn` only.
    const mutate = (fn, from, to) => {
      const start = ino.indexOf(`static esp_err_t ${fn}(`);
      assert.ok(start >= 0, `canary_wap.ino no longer defines ${fn}()`);
      const end = ino.indexOf('\n}\n', start);
      const region = ino.slice(start, end);
      assert.equal(region.split(from).length, 2, `the mutation's anchor is not once in ${fn}(): ${from}`);
      return new Map([...sketch, ['canary_wap.ino',
        ino.slice(0, start) + region.replace(from, to) + ino.slice(end)]]);
    };
    // The receipt sent through http_send_json(), which sets ACAO *.
    const viaHelper = mutate('send_provisioning_receipt',
      'httpd_resp_set_type(req, "application/json");\n  httpd_resp_set_hdr(req, "Cache-Control", "no-store");\n  const esp_err_t ret = httpd_resp_sendstr(req, json);',
      'const esp_err_t ret = http_send_json(req, json);');
    assert.match(receiptCorsFindings(viaHelper).join('\n'), /send_provisioning_receipt\(\) answers through http_send_json\(\)/);
    // The 409 sent through http_send_error(), which calls http_send_json().
    const via409 = mutate('handle_provisioning_receipt',
      'httpd_resp_set_status(req, "409 Conflict");',
      'return http_send_error(req, 409, "tap_contended");');
    assert.match(receiptCorsFindings(via409).join('\n'), /handle_provisioning_receipt\(\) answers through http_send_error\(\)/);
    // The header set by hand on the 403.
    const direct = mutate('handle_provisioning_receipt',
      'httpd_resp_set_status(req, "403 Forbidden");\n    httpd_resp_set_type(req, "application/json");\n    return httpd_resp_sendstr(req, body);',
      'httpd_resp_set_status(req, "403 Forbidden");\n    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");\n    return httpd_resp_sendstr(req, body);');
    assert.match(receiptCorsFindings(direct).join('\n'), /handle_provisioning_receipt\(\) sets Access-Control-Allow-Origin itself/);
    // A 5xx error through the helper is not the pairing's answer.
    assert.ok(/http_send_error\(req, 503, "no_address"\)/.test(ino),
      'send_provisioning_receipt\'s 5xx error path moved; re-check the allowance');
  });

  it('no line of the card says it pairs in this SPA, and one says where it pairs', () => {
    const info = app.DEVICE_TYPES['canary-wap'];
    const lines = [info.tagline, ...info.whatsDifferent, info.proof].filter(Boolean);
    assert.deepEqual(spaPairingClaims(lines), [],
      'the canary-wap card claims SPA pairing its firmware does not answer');
    const where = lines.find((l) => /iPhone app/.test(l) && /recovery kit/.test(l)
      && /Bluetooth/.test(l) && /Home Assistant/.test(l));
    assert.ok(where, 'the canary-wap card must say where a WAP pairs (the iPhone app, from its recovery kit or '
      + 'its Bluetooth setup, and Home Assistant)');
    // The dashboard's own pairing card leads back to this SPA's QR scan, so
    // the dashboard is named only as where the recovery kit comes from.
    assert.ok(!/\bfrom its own dashboard\b|\bpairs? (?:from|on) (?:its|the) dashboard\b/i.test(where),
      'the dashboard does not pair a WAP; its pairing card sends the owner back to this SPA');
  });

  it('the claim check catches the line the card used to carry, and its paraphrases', () => {
    for (const line of [
      'Pairs right here over HTTP with a short tap of its BOOT button',
      'Pairs right here',
      'Pairs with this app over HTTP',
      'Pairs right here, or from its own dashboard or the SecuraCV iPhone app',
      'Pair it from this page with one press; also from its own dashboard or the SecuraCV iPhone app, and Home Assistant — not this app',
      'Scan its dashboard QR in the app',
    ]) {
      assert.equal(spaPairingClaims([line]).length, 1, line);
    }
  });

  it('keeps the http wizard branch: the copy changed, not the flow', () => {
    assert.equal(app.DEVICE_TYPES['canary-wap'].pairing, 'http');
  });
});
