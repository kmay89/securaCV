'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const SPA_PATH = path.join(__dirname, '..', '..', 'spa', 'app.js');

describe('SPA Peer Discovery', () => {
  const js = fs.readFileSync(SPA_PATH, 'utf8');

  it('queries /api/v1/peers from existing devices', () => {
    assert.ok(js.includes("'/api/v1/peers'") || js.includes('"/api/v1/peers"'),
      'SPA must call the peers endpoint to surface discovered Canaries');
  });

  it('renders a Discovered section into the canaries view', () => {
    assert.ok(/discovered-peers/.test(js),
      'SPA must render a discovered-peers container on the canaries view');
    assert.ok(/Discovered on your network/.test(js),
      'SPA must surface a human-readable header for the discovered list');
    assert.ok(/Pair this Canary/.test(js),
      'SPA must offer a one-tap "Pair this Canary" affordance');
  });

  it('builds a peer base_url from mdns_hostname, in one place', () => {
    // mDNS names survive DHCP lease changes; IPs do not. And the served
    // connect-src reaches a peer only by its .local name (the rows below).
    assert.ok(/mdns_hostname/.test(js),
      'SPA must reference peer.mdns_hostname when building the pair URL');
    assert.ok(/peerBaseUrl/.test(js),
      'SPA must centralize peer URL construction');
  });

  it('does NOT add discovered peers without explicit user action', () => {
    // D6 / security model: each device has its own token. Discovery only
    // surfaces candidates; the user (and the per-device token) authorize.
    // The Pair flow must route through the existing add form, not auto-add.
    const hasPairNav = /sessionStorage\.setItem\(['"]canary_prefill_host['"]/.test(js)
                    || /Router\.navigate\(['"]#\/canaries\/add['"]\)/.test(js);
    assert.ok(hasPairNav,
      'Pairing must hand off to the manual add flow, never auto-add');
  });

  it('filters out devices already in the user\'s fleet', () => {
    assert.ok(/collectKnownIdentities/.test(js),
      'SPA must dedupe discovered peers against the existing device list');
  });
});

// ── Behavior: an address the served CSP refuses is never fetched ─────────
//
// The page is served with connect-src 'self' http://*.local
// (device-api/middleware/security-headers.js; tests/security/
// security-headers.test.js holds it). CSP cannot name an IP range, so a
// request to an IP that is not the page's own origin is refused by the
// browser before it leaves, and the page sees the TypeError an unreachable
// device gives (sweep F427). These rows run app.js in a VM sandbox with a
// small fake DOM and a recording fetch, the page opened from
// http://192.168.1.47 (a device's own address), and hold every path that
// takes an address from the owner, a receipt or a peer list to two things
// (sweep F448): an IP other than the page's origin answers "enter its .local
// name" and reaches no fetch, and the page's own IP and a .local name still
// fetch. No URL is built from a peer's `ip`.

const PAGE_ORIGIN = 'http://192.168.1.47';
const REFUSED_IPS = [
  '192.168.1.50',   // RFC 1918, not this page
  '10.0.0.5',       // RFC 1918
  '172.16.4.2',     // RFC 1918
  '169.254.1.1',    // IPv4 link-local
  '127.0.0.1',      // loopback
  '[fe80::1]',      // IPv6 link-local
  '[fd00::1]',      // IPv6 unique-local
  '[::1]',          // IPv6 loopback
  '192.168.1.47:8080', // this page's IP on another port is another origin
];
const LOCAL_NAME = /only by its \.local name, not by an IP address\. Enter its \.local name/;

class FakeNode {
  constructor(tag, text) {
    this.tagName = tag;
    this.children = [];
    this.parentNode = null;
    this.className = '';
    this._text = text === undefined ? '' : String(text);
    this.attrs = {};
    this.listeners = {};
    this.style = {};
    this.value = '';
    this.classList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
  }
  get firstChild() { return this.children[0] || null; }
  appendChild(child) {
    if (child.parentNode) child.parentNode.removeChild(child);
    child.parentNode = this;
    this.children.push(child);
    return child;
  }
  removeChild(child) {
    const i = this.children.indexOf(child);
    if (i >= 0) this.children.splice(i, 1);
    child.parentNode = null;
    return child;
  }
  set textContent(v) {
    this._text = String(v);
    this.children.forEach((c) => { c.parentNode = null; });
    this.children = [];
  }
  get textContent() { return this._text + this.children.map((c) => c.textContent).join(''); }
  addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return this.attrs[k]; }
  querySelector() { return null; }
  click() { (this.listeners.click || []).forEach((fn) => fn({ preventDefault() {} })); }
  all() {
    const out = [];
    const walk = (n) => { out.push(n); n.children.forEach(walk); };
    walk(this);
    return out;
  }
  byClass(cls) { return this.all().filter((n) => String(n.className).split(/\s+/).includes(cls)); }
  byTag(tag) { return this.all().filter((n) => n.tagName === tag); }
  button(text) { return this.byTag('button').find((n) => n.textContent === text); }
}

function memStorage(initial) {
  const store = new Map(Object.entries(initial || {}));
  return {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => { store.set(k, String(v)); },
    removeItem: (k) => { store.delete(k); },
  };
}

// A URL the served connect-src refuses: an IP host that is not this page's
// origin. The stub fetch rejects it the way the browser does (a TypeError,
// sweep F427), so a path that still reaches fetch with one sees what a page
// under the served CSP sees.
function cspRefuses(url) {
  const u = new URL(url);
  return !u.hostname.endsWith('.local') && u.origin !== PAGE_ORIGIN;
}

// `peers` is what GET /api/v1/peers answers; a URL matching `unreachable`
// rejects like a device that is not there; every other route stays in
// flight, so a step that asks shows up in `fetches` and nothing settles it.
// `local` seeds localStorage.
function loadApp({ prefillHost, devices, peers, unreachable, local } = {}) {
  const body = new FakeNode('body');
  const appRoot = body.appendChild(new FakeNode('div'));
  const noop = () => {};
  const fetches = [];
  const document = {
    body: {
      contains(node) {
        for (let n = node; n; n = n.parentNode) if (n === body) return true;
        return false;
      },
    },
    addEventListener: noop,
    getElementById: (id) => (id === 'app' ? appRoot : body.all().find((n) => n.attrs.id === id) || null),
    createElement: (tag) => new FakeNode(tag),
    createElementNS: (_ns, tag) => new FakeNode(tag),
    createTextNode: (text) => new FakeNode('#text', text),
    querySelector: () => null,
  };
  const session = {};
  if (prefillHost) session.canary_prefill_host = prefillHost;
  const sandbox = {
    document,
    window: {
      addEventListener: noop,
      location: { hash: '', origin: PAGE_ORIGIN },
      history: { back: noop },
      matchMedia: () => ({ matches: false, addEventListener: noop }),
    },
    localStorage: memStorage(local),
    sessionStorage: memStorage(session),
    navigator: { onLine: true },
    setInterval: noop,
    clearInterval: noop,
    setTimeout: () => 0,
    clearTimeout: noop,
    fetch: (url, opts) => {
      fetches.push({ url: String(url), method: (opts && opts.method) || 'GET' });
      if (cspRefuses(String(url)) || (unreachable && unreachable.test(String(url)))) {
        return Promise.reject(new TypeError('Failed to fetch'));
      }
      if (/\/api\/v1\/peers$/.test(String(url))) {
        return Promise.resolve({ status: 200, ok: true, json: () => Promise.resolve({ peers: peers || [] }) });
      }
      return new Promise(noop);
    },
    AbortController,
    URL,
    console,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(SPA_PATH, 'utf8'), sandbox);
  if (devices) sandbox.CanaryStorage.saveDevices(devices);
  return { app: sandbox, appRoot, fetches };
}

function settle() { return new Promise((r) => setImmediate(r)); }
async function settleAll() { for (let i = 0; i < 5; i++) await settle(); }

function alertText(h) {
  return h.appRoot.byClass('alert-error').map((n) => n.textContent).join(' | ');
}
function titles(h) {
  return h.appRoot.byClass('pair-step-title').map((n) => n.textContent);
}
// A fetch to an IP host that is not this page's origin: what CSP refuses.
function refusedFetches(h) {
  return h.fetches.filter((f) => cspRefuses(f.url));
}

describe('CanaryAPI.request: the backstop for every path', () => {
  for (const host of REFUSED_IPS) {
    it(`refuses http://${host} without fetching it`, async () => {
      const h = loadApp();
      // fetch() is called synchronously inside request(), so the record is
      // complete here (and the stub's answer never settles: no await first).
      const asked = h.app.CanaryAPI.request('http://' + host, '/api/v1/info');
      assert.deepEqual(h.fetches, [], 'a request CSP refuses reached fetch');
      await assert.rejects(asked,
        (err) => err.code === 'needs_local_name' && LOCAL_NAME.test(err.message));
    });
  }

  it("fetches this page's own IP and a .local name", () => {
    const h = loadApp();
    h.app.CanaryAPI.request(PAGE_ORIGIN, '/api/v1/info');
    h.app.CanaryAPI.request('http://canary-b1c2.local', '/api/v1/info');
    assert.deepEqual(h.fetches.map((f) => f.url),
      [PAGE_ORIGIN + '/api/v1/info', 'http://canary-b1c2.local/api/v1/info']);
  });
});

describe('the Add Canary entry paths answer "enter its .local name" for an IP', () => {
  function chooseStep() {
    const h = loadApp();
    h.app.renderAddCanaryView();
    assert.deepEqual(titles(h), ['Power up your Canary']);
    const input = h.appRoot.byTag('input')[0];
    return { h, input, cont: h.appRoot.button('Continue') };
  }

  it('the address placeholders suggest a .local name and no IP', () => {
    const { h } = chooseStep();
    const placeholders = h.appRoot.byTag('input').map((n) => n.attrs.placeholder).filter(Boolean);
    assert.ok(placeholders.includes('e.g. canary-a3f7.local'), JSON.stringify(placeholders));
    assert.deepEqual(placeholders.filter((p) => /\d+\.\d+\.\d+\.\d+|\[[0-9a-f:]+\]/i.test(p)), [],
      'a placeholder suggests an IP the served CSP refuses');
  });

  for (const host of REFUSED_IPS) {
    it(`manual entry: ${host} gets the message and no capture`, async () => {
      const { h, input, cont } = chooseStep();
      input.value = host;
      cont.click();
      await settleAll();
      assert.match(alertText(h), LOCAL_NAME);
      assert.deepEqual(titles(h), ['Power up your Canary'], 'the capture step started for ' + host);
      assert.deepEqual(h.fetches, []);
    });
  }

  for (const [host, url] of [['192.168.1.47', PAGE_ORIGIN], ['canary-a3f7.local', 'http://canary-a3f7.local']]) {
    it(`manual entry: ${host} still starts the capture and asks it`, async () => {
      const { h, input, cont } = chooseStep();
      input.value = host;
      cont.click();
      await settleAll();
      assert.equal(alertText(h), '');
      assert.deepEqual(titles(h), ['Asking the Canary first']);
      assert.deepEqual(h.fetches.map((f) => f.url), [url + '/api/provisioning-receipt']);
    });
  }

  it('"Add with token": an IP gets the message and no request', async () => {
    for (const host of REFUSED_IPS) {
      const { h } = chooseStep();
      const add = h.appRoot.button('Add with token');
      const [hostInput, tokenInput] = add.parentNode.byTag('input');
      hostInput.value = host;
      tokenInput.value = 'cv_a3f7_x';
      add.click();
      await settleAll();
      assert.match(alertText(h), LOCAL_NAME, host);
      assert.deepEqual(h.fetches, [], host);
    }
  });

  it('"Add with token": a .local name and this page\'s IP still ask /api/v1/info', async () => {
    for (const [host, url] of [['canary-a3f7.local', 'http://canary-a3f7.local'], ['192.168.1.47', PAGE_ORIGIN]]) {
      const { h } = chooseStep();
      const add = h.appRoot.button('Add with token');
      const [hostInput, tokenInput] = add.parentNode.byTag('input');
      hostInput.value = host;
      tokenInput.value = 'cv_a3f7_x';
      add.click();
      await settleAll();
      assert.equal(alertText(h), '', host);
      assert.deepEqual(h.fetches.map((f) => f.url), [url + '/api/v1/info'], host);
    }
  });

  it('a pasted receipt naming an IP gets the message and no request', async () => {
    for (const key of ['base_url', 'host']) {
      for (const host of REFUSED_IPS) {
        const { h } = chooseStep();
        const add = h.appRoot.button('Add from receipt');
        const textarea = add.parentNode.byTag('textarea')[0];
        textarea.value = JSON.stringify({ device_id: 'canary-b1c2', [key]: 'http://' + host, token: 'cv_b1c2_x' });
        add.click();
        await settleAll();
        assert.match(alertText(h), LOCAL_NAME, key + ' ' + host);
        assert.deepEqual(h.fetches, [], key + ' ' + host);
      }
    }
  });

  it('a prefilled IP that is not this page lands on the entry step with the message', async () => {
    for (const host of REFUSED_IPS) {
      const h = loadApp({ prefillHost: host });
      h.app.renderAddCanaryView();
      await settleAll();
      assert.deepEqual(titles(h), ['Power up your Canary'], host);
      assert.match(alertText(h), LOCAL_NAME, host);
      assert.deepEqual(h.fetches, [], host);
    }
  });
});

describe('peers: no URL is built from a peer\'s ip', () => {
  const NAMED = { device_id: 'canary-b1c2', name: 'Garage', ip: '192.168.1.103', mdns_hostname: 'canary-b1c2.local' };
  const IP_ONLY = { device_id: 'canary-c3d4', name: 'Porch', ip: '192.168.1.104' };
  const IP_AS_NAME = { device_id: 'canary-c3d5', name: 'Shed', mdns_hostname: '192.168.1.105', ip: '192.168.1.105' };
  const SENSE_IP_ONLY = { device_id: 'canary-e5f6', name: 'Yard', dt: 'canary-sense', ip: '192.168.1.110' };
  const PEERS = [NAMED, IP_ONLY, IP_AS_NAME, SENSE_IP_ONLY];
  const KNOWN = [{ id: 'canary-a3f7', name: 'Hall', base_url: 'http://canary-a3f7.local', token: 'cv_a3f7_x' }];
  const NOT_BY_NAME = /^Not reachable by name: it lists no \.local name/;

  it('peerBaseUrl reads only a .local mdns_hostname', () => {
    const h = loadApp();
    assert.equal(h.app.peerBaseUrl(NAMED), 'http://canary-b1c2.local');
    assert.equal(h.app.peerBaseUrl({ mdns_hostname: 'http://canary-b1c2.local/' }), 'http://canary-b1c2.local');
    assert.equal(h.app.peerBaseUrl(IP_ONLY), '', 'a URL was built from peer.ip');
    assert.equal(h.app.peerBaseUrl(IP_AS_NAME), '', 'an IP in mdns_hostname is not a .local name');
    assert.equal(h.app.peerBaseUrl({ ip: PAGE_ORIGIN.slice(7) }), '', 'not even this page\'s IP comes from peer.ip');
    assert.equal(h.app.peerBaseUrl({}), '');
  });

  it('a peer with no .local name stays on the discovered list', async () => {
    const h = loadApp({ devices: KNOWN, peers: PEERS });
    const found = await h.app.fetchDiscoveredPeers();
    assert.deepEqual([...found].map((p) => p.device_id), PEERS.map((p) => p.device_id));
  });

  it('the Discovered section shows an unnamed HTTP peer as not reachable by name, with no Pair button', () => {
    const h = loadApp();
    const section = h.app.renderDiscoveredPeerSection(PEERS);
    const rows = section.children.filter((n) => n.className === 'card');
    assert.equal(rows.length, PEERS.length);
    const byId = Object.fromEntries(PEERS.map((p, i) => [p.device_id, rows[i]]));
    for (const id of ['canary-c3d4', 'canary-c3d5']) {
      assert.equal(byId[id].button('Pair this Canary'), undefined, id + ' offers a pairing CSP refuses');
      assert.match(byId[id].byClass('alert-warning').map((n) => n.textContent).join(''), NOT_BY_NAME, id);
    }
    assert.ok(byId['canary-b1c2'].button('Pair this Canary'), 'a named peer keeps its Pair button');
    assert.ok(byId['canary-e5f6'].button('Pair this Canary'), 'an MQTT peer pairs through Home Assistant');
    assert.doesNotMatch(section.textContent, /192\.168\.1\.1\d\d/, 'a peer IP is shown as its address');
  });

  it('"Pair this Canary" hands the wizard the .local URL', () => {
    const h = loadApp();
    const section = h.app.renderDiscoveredPeerSection([NAMED]);
    section.children.find((n) => n.className === 'card').button('Pair this Canary').click();
    assert.equal(h.app.sessionStorage.getItem('canary_prefill_host'), 'http://canary-b1c2.local');
  });

  it('the wizard lists every peer, captures from a .local name, and never from an IP', async () => {
    const h = loadApp({ devices: KNOWN, peers: PEERS });
    h.app.renderAddCanaryView();
    await settleAll();
    const rows = h.appRoot.byClass('card').filter((n) => n.byClass('card-title').length === 1
      && PEERS.some((p) => p.name === n.byClass('card-title')[0].textContent));
    assert.equal(rows.length, PEERS.length, 'a discovered peer vanished from the wizard');
    const row = (name) => rows.find((n) => n.byClass('card-title')[0].textContent === name);
    for (const name of ['Porch', 'Shed']) {
      assert.equal((row(name).listeners.click || []).length, 0, name + ' takes a click into a capture CSP refuses');
      assert.match(row(name).byClass('alert-warning').map((n) => n.textContent).join(''), NOT_BY_NAME, name);
    }
    assert.doesNotMatch(h.appRoot.textContent, /192\.168\.1\.1\d\d/, 'a peer IP is shown as its address');
    assert.equal(row('Yard').byClass('alert-warning').length, 0, 'an MQTT peer is called not reachable by name');
    assert.equal((row('Yard').listeners.click || []).length, 1, 'Yard (sense, no .local name) lost its click');
    row('Garage').click();
    await settleAll();
    assert.deepEqual(titles(h), ['Asking the Canary first']);
    assert.deepEqual(h.fetches.map((f) => f.url),
      ['http://canary-a3f7.local/api/v1/peers', 'http://canary-b1c2.local/api/provisioning-receipt']);
    assert.deepEqual(refusedFetches(h), []);
  });
});

describe('an MQTT peer with no .local name keeps its Home Assistant route in the wizard', () => {
  it('Yard (canary-sense, ip only) takes its click into the Home Assistant steps, asking nothing', async () => {
    const yard = { device_id: 'canary-e5f6', name: 'Yard', dt: 'canary-sense', ip: '192.168.1.110' };
    const h = loadApp({
      devices: [{ id: 'canary-a3f7', name: 'Hall', base_url: 'http://canary-a3f7.local', token: 'cv_a3f7_x' }],
      peers: [yard],
    });
    h.app.renderAddCanaryView();
    await settleAll();
    const row = h.appRoot.byClass('card').find((n) => n.byClass('card-title').length === 1
      && n.byClass('card-title')[0].textContent === 'Yard');
    assert.ok(row, 'Yard vanished from the wizard');
    assert.equal(row.byClass('alert-warning').length, 0, 'an MQTT peer is called not reachable by name');
    const asked = h.fetches.length;
    row.click();
    await settleAll();
    assert.deepEqual(titles(h), ['Yard'], 'Yard did not reach its Home Assistant step');
    assert.ok(h.appRoot.byClass('card-title').some((n) => n.textContent === 'It joins through Home Assistant'));
    assert.equal(h.fetches.length, asked, 'the Home Assistant step made a request');
    assert.deepEqual(refusedFetches(h), []);
  });
});

// ── A device already stored under an IP the served CSP refuses ───────────
//
// Pairing no longer stores one, but a page once served without this CSP (or
// a browser that did not enforce it) may have. CanaryAPI.request() refuses
// it without asking, and each view that names the failure says why instead
// of calling it offline. The stub fetch rejects a CSP-refused URL the way the
// browser does, so on code that still asks, the page sees what it would see.

describe('a device stored under a refused IP', () => {
  const ATTIC = { id: 'canary-9999', name: 'Attic', base_url: 'http://10.0.0.5', token: 'cv_9999_x' };
  const CELLAR = { id: 'canary-b1c2', name: 'Cellar', base_url: 'http://canary-b1c2.local', token: 'cv_b1c2_x' };
  const UNREACHABLE = /^http:\/\/canary-b1c2\.local\//;
  const HINT_DISMISSED = { cv_coverage_hint_dismissed: '1' };

  function cardLabel(h, name) {
    const card = h.appRoot.byClass('fleet-device-card')
      .find((n) => n.byClass('card-title')[0].textContent === name);
    assert.ok(card, name + ' has no fleet card');
    return card.byClass('fleet-device-status')[0].textContent;
  }

  it('its fleet card reads "Needs its .local name", and an unanswered one still reads Offline', async () => {
    const h = loadApp({ devices: [ATTIC, CELLAR], unreachable: UNREACHABLE, local: HINT_DISMISSED });
    h.app.renderCanariesView();
    await settleAll();
    assert.equal(cardLabel(h, 'Attic'), 'Needs its .local name');
    assert.equal(cardLabel(h, 'Cellar'), 'Offline');
    assert.deepEqual(refusedFetches(h), [], 'a request CSP refuses reached fetch');
  });

  it('its device view says to remove it and add it again by name, with no Retry', async () => {
    const h = loadApp({ devices: [ATTIC], local: HINT_DISMISSED });
    h.app.renderDeviceView('canary-9999');
    await settleAll();
    const alert = h.appRoot.all().find((n) => n.attrs.id === 'device-alert');
    assert.match(alert.textContent,
      /^Saved under an IP address this app cannot reach: .*Remove it below and add it again by its \.local name/);
    assert.equal(alert.button('Retry'), undefined, 'Retry would be refused the same way');
    assert.ok(h.appRoot.button('Remove Device'), 'the view names a Remove button it does not have');
    assert.deepEqual(refusedFetches(h), [], 'a request CSP refuses reached fetch');
  });

  it('a device that does not answer still reads "Device offline." with Retry', async () => {
    const h = loadApp({ devices: [CELLAR], unreachable: UNREACHABLE, local: HINT_DISMISSED });
    h.app.renderDeviceView('canary-b1c2');
    await settleAll();
    const alert = h.appRoot.all().find((n) => n.attrs.id === 'device-alert');
    assert.equal(alert.textContent, 'Device offline.Retry');
    assert.ok(alert.button('Retry'));
  });

  it('the Events summary names it apart from a device that did not answer', async () => {
    const h = loadApp({ devices: [ATTIC, CELLAR], unreachable: UNREACHABLE, local: HINT_DISMISSED });
    const data = await h.app.fetchAllWitnessRecords();
    const scroller = h.appRoot.appendChild(new FakeNode('div'));
    const container = scroller.appendChild(new FakeNode('div'));
    h.app.renderEventsContent(container, data);
    const notes = container.byClass('events-offline-note').map((n) => n.textContent);
    assert.deepEqual(notes, ['⚠ Could not reach: Cellar', '⚠ Needs its .local name: Attic']);
    assert.deepEqual(refusedFetches(h), [], 'a request CSP refuses reached fetch');
  });
});
