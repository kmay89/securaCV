'use strict';

// Unit tests for the onboarding wizard's pure decision logic.
//
// The token handling and connect-retry decisions live in a pure, DOM-free
// block inside companion_pwa.h, fenced by `WIZARD_LOGIC:BEGIN/END` markers
// and self-exported via `module.exports` under Node (same pattern as
// selftest_ui.test.js). We extract that exact block and evaluate it here,
// so these tests exercise the SAME source the firmware ships.
//
//   node --test firmware/projects/canary-wap/arduino/canary_wap/wizard_logic.test.js

const { describe, it, before } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const PWA = path.join(__dirname, 'companion_pwa.h');

function loadLogic() {
  const src = fs.readFileSync(PWA, 'utf8');
  const m = src.match(/\/\*\s*WIZARD_LOGIC:BEGIN[\s\S]*?WIZARD_LOGIC:END\s*\*\//);
  if (!m) throw new Error('WIZARD_LOGIC block not found in companion_pwa.h');
  const sandbox = { module: { exports: {} } };
  vm.runInNewContext(m[0], sandbox, { filename: 'wizard_logic.extracted.js' });
  const L = sandbox.module.exports;
  if (!L || typeof L.connectOutcome !== 'function') {
    throw new Error('extracted block did not export the expected WizardLogic API');
  }
  return L;
}

let L;
before(() => { L = loadLogic(); });

describe('isPairToken', () => {
  it('accepts exactly 64 hex chars', () => {
    assert.ok(L.isPairToken('a'.repeat(64)));
    assert.ok(L.isPairToken('0123456789abcdefABCDEF'.padEnd(64, '0')));
  });
  it('rejects everything else', () => {
    assert.ok(!L.isPairToken(null));
    assert.ok(!L.isPairToken(undefined));
    assert.ok(!L.isPairToken(''));
    assert.ok(!L.isPairToken('a'.repeat(63)));
    assert.ok(!L.isPairToken('a'.repeat(65)));
    assert.ok(!L.isPairToken('g'.repeat(64)));   // non-hex
    assert.ok(!L.isPairToken(42));
  });
});

describe('connectOutcome', () => {
  it('proceeds on an accepted submit', () => {
    // Field-wise compare: the object comes from another vm realm, so
    // deepStrictEqual would fail on prototype identity alone.
    const out = L.connectOutcome(true, { ok: true }, false);
    assert.equal(out.action, 'proceed');
    assert.equal(out.isTokenErr, false);
  });
  it('retries with a fresh token on the FIRST invalid_token', () => {
    const out = L.connectOutcome(true, { ok: false, code: 'invalid_token' }, false);
    assert.equal(out.action, 'refresh-retry');
    assert.equal(out.isTokenErr, true);
  });
  it('does not loop: the SECOND invalid_token is a failure', () => {
    const out = L.connectOutcome(true, { ok: false, code: 'invalid_token' }, true);
    assert.equal(out.action, 'fail');
    assert.equal(out.isTokenErr, true);
  });
  it('non-token errors fail without a retry', () => {
    const out = L.connectOutcome(true, { ok: false, error: 'Invalid SSID' }, false);
    assert.equal(out.action, 'fail');
    assert.equal(out.isTokenErr, false);
  });
  it('HTTP-level failure without a body is a plain failure', () => {
    const out = L.connectOutcome(false, null, false);
    assert.equal(out.action, 'fail');
    assert.equal(out.isTokenErr, false);
  });
  it('a malformed 200 body is NOT success — ok must be explicitly true', () => {
    for (const body of [{}, { ok: 'yes' }, [1, 2], 'ok', { message: 'hi' }]) {
      const out = L.connectOutcome(true, body, false);
      assert.equal(out.action, 'fail', JSON.stringify(body));
    }
  });
});

describe('capabilityNotice', () => {
  it('is fully hidden in wizard (token) mode — Web Bluetooth is irrelevant there', () => {
    assert.equal(L.capabilityNotice(true, false, false).show, false);
    assert.equal(L.capabilityNotice(true, true, true).show, false);
  });
  it('insecure origin is an informational note, and never mentions failure', () => {
    const n = L.capabilityNotice(false, false, false);
    assert.equal(n.show, true);
    assert.match(n.text, /work fine/i);
    assert.doesNotMatch(n.text, /insecure origin/i);
  });
  it('missing Web Bluetooth points at Bluefy', () => {
    const n = L.capabilityNotice(false, true, false);
    assert.equal(n.show, true);
    assert.match(n.text, /Bluefy/);
  });
  it('capable browser gets no banner', () => {
    assert.equal(L.capabilityNotice(false, true, true).show, false);
  });
});

describe('connectBody (household time zone seed, repo sweep F28)', () => {
  it('carries the phone zone as tz_iana beside the credentials', () => {
    const b = L.connectBody('Home', 'pw', 'a'.repeat(64), 'America/New_York');
    assert.deepEqual(Object.keys(b).sort(), ['password', 'ssid', 'token', 'tz_iana']);
    assert.equal(b.tz_iana, 'America/New_York');
    assert.equal(b.ssid, 'Home');
    assert.equal(b.password, 'pw');
  });
  it('accepts the table shapes: three-part names, UTC, Etc/UTC', () => {
    for (const z of ['America/Argentina/Buenos_Aires', 'UTC', 'Etc/UTC', 'Europe/Kyiv']) {
      assert.equal(L.connectBody('s', '', 't', z).tz_iana, z, z);
    }
  });
  it('leaves the zone out when the browser could not tell, or sent something odd — the join never depends on it', () => {
    for (const z of ['', undefined, null, 42, 'America/New York', '../etc', 'Europe/"x"',
                     'a'.repeat(48), '/Europe', 'Europe/']) {
      const b = L.connectBody('Home', 'pw', 't', z);
      assert.ok(!('tz_iana' in b), JSON.stringify(z));
      assert.deepEqual(Object.keys(b).sort(), ['password', 'ssid', 'token']);
    }
  });
});

describe('tzNotice (what the success card says about the zone, repo sweep F28)', () => {
  it('names the fallback when the Canary does not know the phone zone', () => {
    const t = L.tzNotice('unknown_zone', 'Africa/Johannesburg');
    assert.match(t, /Africa\/Johannesburg/);
    assert.match(t, /world time \(UTC\)/);
  });
  it('says the zone was not stored when the device could not store it', () => {
    const t = L.tzNotice('not_set', 'Europe/Kyiv');
    assert.match(t, /couldn't store/);
    assert.match(t, /Europe\/Kyiv/);
    assert.match(t, /UTC/);
  });
  it('still reads as a sentence when the phone gave no name', () => {
    assert.match(L.tzNotice('unknown_zone', ''), /^Your phone's time zone isn't/);
  });
  it('says nothing when the zone was set, none was sent, or an older firmware answered without "tz"', () => {
    for (const o of ['set', 'not_sent', undefined, null, '', 'something-new']) {
      assert.equal(L.tzNotice(o, 'America/New_York'), '', String(o));
    }
  });
});

describe('closeOutHost (the host the close-out link opens, repo sweep F129)', () => {
  it('is the mdns_host /api/device-info names: canary-<4 hex> unnamed, canary-<name> once named', () => {
    // the repo test key: device id canary-s3-4dC2, fp 7916ca48... -> canary-7916
    assert.equal(L.closeOutHost({ device_id: 'canary-s3-4dC2', mdns_host: 'canary-7916' }), 'canary-7916');
    assert.equal(L.closeOutHost({ device_id: 'canary-s3-4dC2', mdns_host: 'canary-kitchen' }), 'canary-kitchen');
  });
  it('never builds a host from the device id', () => {
    assert.equal(L.closeOutHost({ device_id: 'canary-s3-4dC2' }), '');
  });
  it('takes one RFC 1123 label and nothing else, so the static canary.local link stays', () => {
    for (const bad of [undefined, null, {}, { mdns_host: '' }, { mdns_host: 42 },
                       { mdns_host: 'Canary-7916' }, { mdns_host: 'canary-7916.local' },
                       { mdns_host: 'http://canary-7916' }, { mdns_host: '-canary' }, { mdns_host: 'canary-' },
                       { mdns_host: 'canary 7916' }, { mdns_host: 'a'.repeat(64) }]) {
      assert.equal(L.closeOutHost(bad), '', JSON.stringify(bad));
    }
    assert.equal(L.closeOutHost({ mdns_host: 'a'.repeat(63) }), 'a'.repeat(63));
  });
});

describe('the close-out link, on the routes the device serves (repo sweep F129)', () => {
  const INO = path.join(__dirname, 'canary_wap.ino');

  // updateMdnsLinkFromDevice exactly as companion_pwa.h ships it, run against
  // a fake device that answers the way canary_wap.ino's routes do: the page
  // holds no API token, so the auth-wrapped /api/status answers 401 unless
  // the browser holds the cv_session cookie the recovery-kit save issues
  // (api_auth_check accepts that cookie first), when it answers with
  // handle_status's device_id; the public /api/device-info answers with
  // handle_device_info's JSON either way.
  function linkAfter(info, { status = 200, throws = false, cookie = false } = {}) {
    const src = fs.readFileSync(PWA, 'utf8');
    const m = src.match(/\n {2}async function updateMdnsLinkFromDevice\(\) \{[\s\S]*?\n {2}\}\n/);
    if (!m) throw new Error('updateMdnsLinkFromDevice not found in companion_pwa.h');
    // with any helper function of the page's that it calls, so the harness
    // runs whatever page it is given as the browser would
    let helpers = '';
    for (const name of new Set([...m[0].matchAll(/\b([A-Za-z_$][\w$]*)\(/g)].map((c) => c[1]))) {
      const h = src.match(new RegExp('\\n {2}function ' + name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\([^)]*\\) \\{[\\s\\S]*?\\n {2}\\}\\n'));
      if (h && name !== 'updateMdnsLinkFromDevice') helpers += h[0];
    }
    const link = { href: 'http://canary.local/', textContent: 'Open canary.local' };
    const asked = [];
    const fetch = async (url, opts) => {
      asked.push(url);
      if (throws) throw new Error('network blip');
      const authed = cookie || !!(opts && opts.headers && opts.headers.Authorization);
      if (url === '/api/status' && !authed) return { ok: false, status: 401, json: async () => ({ error: 'unauthorized' }) };
      if (url === '/api/status') return { ok: true, status: 200, json: async () => ({ device_id: info.device_id }) };
      if (url === '/api/device-info') return { ok: status === 200, status, json: async () => info };
      return { ok: false, status: 404, json: async () => ({}) };
    };
    const ctx = { $w: (id) => (id === 'wiz-link-mdns' ? link : null), fetch, WizardLogic: L };
    const fn = vm.runInNewContext(helpers + m[0] + '\n;updateMdnsLinkFromDevice', ctx, { filename: 'close_out_link.extracted.js' });
    return fn().then(() => ({ link, asked }));
  }

  // handle_device_info's own JSON, as the unnamed test-key device sends it
  const INFO = { device_id: 'canary-s3-4dC2', device_name: '', mdns_host: 'canary-7916',
                 firmware: '2.4.15', pubkey_fp: '7916ca487912fa1b', auth_required: true };

  it('the device serves mdns_host on a public route and gates /api/status', () => {
    const ino = fs.readFileSync(INO, 'utf8');
    assert.match(ino, /\.uri = "\/api\/device-info", \.method = HTTP_GET, \.handler = handle_device_info \}/);
    assert.match(ino, /\.uri = "\/api\/status", \.method = HTTP_GET, \.handler = handle_status_auth \}/);
    const body = ino.split('static esp_err_t handle_device_info(httpd_req_t* req) {')[1].split('\n}\n')[0];
    assert.ok(!/api_auth_check/.test(body), 'handle_device_info takes no token');
    // The answer is identity_json.h's (sweep F212): the host the device
    // advertises goes in as mdns_host and comes out under that key.
    assert.match(body, /in\.mdns_host\s*=\s*g_device\.mdns_hostname;/);
    assert.match(body, /identity_json::device_info\(in, json, need\);/);
    const idj = fs.readFileSync(path.join(__dirname, 'identity_json.h'), 'utf8');
    assert.match(idj, /raw\(w, ",\\"mdns_host\\":"\);\n  str\(w, in\.mdns_host\);/);
  });
  it('opens the host the device advertises, not its device id', async () => {
    const { link, asked } = await linkAfter(INFO);
    assert.deepEqual(asked, ['/api/device-info'], 'one public fetch, no token needed');
    assert.equal(link.href, 'http://canary-7916.local/');
    assert.equal(link.textContent, 'Open canary-7916.local');
    assert.notEqual(link.href, 'http://' + INFO.device_id.toLowerCase() + '.local/');
  });
  it('the recovery-kit save issues a session cookie that /api/status accepts', () => {
    const ino = fs.readFileSync(INO, 'utf8');
    const receipt = ino.split('static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {')[1].split('\n}\n')[0];
    assert.match(receipt, /"cv_session=%s; HttpOnly; SameSite=Strict; Path=\/; Max-Age=86400"/);
    assert.match(receipt, /httpd_resp_set_hdr\(req, "Set-Cookie", cookie_hdr\)/);
    assert.match(ino, /static esp_err_t handle_status_auth\(httpd_req_t\* req\) \{\n  if \(!api_auth_check\(req, g_device\.api_token_str\)\) return ESP_OK;/);
    const auth = fs.readFileSync(path.join(__dirname, 'api_auth.h'), 'utf8');
    const check = auth.split('static bool api_auth_check(httpd_req_t* req, const char* expected_token) {')[1];
    assert.ok(check.indexOf('if (cv_session_validate && cv_session_validate(req)) {') < check.indexOf('auth_is_locked_out()'),
      'the session cookie authenticates before any token check');
    // "Run again" re-runs the self-test, which prepares the close-out links again
    const pwa = fs.readFileSync(PWA, 'utf8');
    assert.match(pwa, /\$w\('wiz-st-rerun'\)\.addEventListener\('click', runSelfTest\);/);
  });
  it('after the recovery-kit save (session cookie), still opens the advertised host, not the device id', async () => {
    const { link, asked } = await linkAfter(INFO, { cookie: true });
    // the page before F129 read /api/status here and linked the device id, lowercased
    assert.equal(link.href, 'http://canary-7916.local/');
    assert.notEqual(link.href, 'http://' + INFO.device_id.toLowerCase() + '.local/');
    assert.deepEqual(asked, ['/api/device-info']);
  });
  it('a named Canary opens canary-<name>.local', async () => {
    const { link } = await linkAfter({ ...INFO, device_name: 'Kitchen', mdns_host: 'canary-kitchen' });
    assert.equal(link.href, 'http://canary-kitchen.local/');
  });
  it('keeps the static canary.local link on an error, a blip or an unusable host', async () => {
    for (const [info, opts] of [[INFO, { status: 500 }], [INFO, { throws: true }],
                                [{ ...INFO, mdns_host: '' }, {}], [{ device_id: INFO.device_id }, {}]]) {
      const { link } = await linkAfter(info, opts);
      assert.equal(link.href, 'http://canary.local/', JSON.stringify([info, opts]));
      assert.equal(link.textContent, 'Open canary.local');
    }
  });
});
