'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

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

  // The served CSP has style-src 'self', which refuses a style attribute
  // (style-src-attr): Chromium drops it and logs a violation, so the layout
  // it carried never shows. app.js once set sixteen through el()'s `style:`
  // key, among them the Settings view's Quiet Hours row and the fleet card's
  // RSSI chip (sweep F474). A view's layout is now a class in styles.css.
  // This row refuses the ways to a style attribute that app.js's source
  // shows: a `style:` key (el() turns any key it does not know into an
  // attribute), setAttribute('style', ...) or toggleAttribute('style'), a
  // ['style'] key, an assignment to cssText or to node.style itself (the
  // same declarations as a string), and parsing markup, which can carry a
  // style="" of its own. It also refuses the paths that hide an attribute's
  // name from such a scan, which app.js has no use for: an attribute node
  // (createAttribute, setAttributeNode) and setAttribute reached through
  // .call, .apply or .bind. A name assembled at run time (node['set' +
  // 'Attribute']) is out of a static scan's reach; setAttr(), which el()
  // and the device-type icons use for every attribute a variable names,
  // throws on a style name (the row below).
  // Writing one property through the CSSOM (node.style.color = ...) is not
  // an attribute, and the CSP allows it.
  it("app.js sets no style attribute, which the served style-src 'self' refuses", () => {
    const securityHeaders = require('../../device-api/middleware/security-headers');
    const headers = {};
    securityHeaders()({}, { setHeader: (k, v) => { headers[k.toLowerCase()] = v; } }, () => {});
    const styleSrc = headers['content-security-policy'].split(';').map((d) => d.trim().split(/\s+/))
      .find((d) => d[0] === 'style-src');
    assert.deepEqual(styleSrc.slice(1), ["'self'"],
      'style-src changed: this row and setAttr() in spa/app.js are written for style-src \'self\'');

    const js = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8');
    const lineOf = (i) => js.slice(0, i).split('\n').length;
    const found = [];
    for (const [what, re] of [
      ['a style: key', /(?:^|[{,(\s])['"]?style['"]?\s*:/gm],
      ["setAttribute('style')", /(?:set|toggle)Attribute\(\s*['"`]style['"`]|setAttributeNS\([^,)]*,\s*['"`]style['"`]/gi],
      ['an attribute node', /\b(?:createAttribute|setAttributeNode)(?:NS)?\b/g],
      ['setAttribute by .call, .apply or .bind', /\b(?:set|toggle)Attribute(?:NS)?\s*\.\s*(?:call|apply|bind)\b/g],
      ["a ['style'] key", /\[\s*['"`]style['"`]\s*\]/g],
      ['cssText', /\bcssText\b/g],
      ['an assignment to .style', /\.style\s*=(?!=)/g],
      ['markup parsing', /\binsertAdjacentHTML\b|\.outerHTML\b|\bDOMParser\b|\bcreateContextualFragment\b|\.srcdoc\b/g],
    ]) {
      for (const m of js.matchAll(re)) found.push(what + ' at app.js:' + lineOf(m.index + m[0].length - 1));
    }
    assert.deepEqual(found, [], found.length + ' way(s) to a style attribute the served CSP refuses: ' +
      'move the declarations to a class in spa/styles.css');

    // An attribute named by a variable is set only by setAttr(), which
    // refuses a style name; a literal name is read by the rows above.
    const byVariable = [...js.matchAll(/\.(?:set|toggle)Attribute(?:NS)?\(\s*(?!['"`])/g)].map((m) => lineOf(m.index));
    const setAttr = js.indexOf('function setAttr(node, name, value) {');
    assert.ok(setAttr >= 0, 'setAttr() is gone');
    assert.deepEqual(byVariable, [lineOf(js.indexOf('node.setAttribute(name, value);', setAttr))],
      'an attribute named by a variable is set outside setAttr()');

    const html = fs.readFileSync(path.join(SPA_DIR, 'index.html'), 'utf8');
    assert.doesNotMatch(html, /\sstyle\s*=|<style\b/i, 'index.html carries an inline style');
  });

  it('el() and the device-type icons refuse a style attribute when they run', () => {
    const made = [];
    const node = (tag) => {
      const n = { tag, attrs: {}, children: [], setAttribute(k, v) { this.attrs[k] = String(v); },
        addEventListener() {}, appendChild(c) { this.children.push(c); return c; } };
      made.push(n);
      return n;
    };
    const noop = () => {};
    const sandbox = {
      document: { addEventListener: noop, getElementById: () => null, createElement: node,
        createElementNS: (_ns, tag) => node(tag), createTextNode: (t) => ({ t }) },
      window: { addEventListener: noop, location: { hash: '' }, history: {},
        matchMedia: () => ({ matches: false, addEventListener: noop }) },
      localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
      sessionStorage: { getItem: () => null, setItem: noop, removeItem: noop },
      navigator: { onLine: true },
      setInterval: noop, clearInterval: noop, setTimeout: noop, clearTimeout: noop,
      fetch: noop, console,
    };
    sandbox.globalThis = sandbox;
    vm.createContext(sandbox);
    vm.runInContext(fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8'), sandbox);
    const app = sandbox;
    const refused = /style-src 'self'/;

    for (const name of ['style', 'Style', 'STYLE']) {
      assert.throws(() => app.el('div', { [name]: 'margin-top: 1rem' }), refused, name);
    }
    assert.throws(() => app.deviceTypeIcon({ icon: [{ tag: 'rect', attrs: { style: 'fill: #000' } }] }),
      refused);
    const div = app.el('div', { className: 'card', title: 'Garage', 'data-room': 'garage' });
    assert.deepEqual(div.attrs, { title: 'Garage', 'data-room': 'garage' });
    for (const [dt, info] of Object.entries(app.DEVICE_TYPES)) {
      if (info.icon) assert.ok(app.deviceTypeIcon(info), dt + ' has no icon');
    }
    assert.ok(made.every((n) => !Object.keys(n.attrs).some((k) => k.toLowerCase() === 'style')));
  });

  // The other half of F474: the layout comes back. Each class that took the
  // place of an inline style is declared in a top-level rule of styles.css
  // (not under @media, so it holds at every width) with exactly the
  // declarations the style attribute carried, and app.js puts it on the same
  // elements, as many times as it once set the style. The source fragments
  // show the compound or ancestor class each selector needs. Whether each
  // rule then wins the cascade over the rules above it was proven in
  // headless Chromium (computed styles and boxes equal to the inline
  // layout's), not here.
  it('the classes that replaced the inline styles are declared and used', () => {
    const LAYOUT = [
      ['.btn-stacked', { 'margin-top': '0.5rem' }, [["className: 'btn btn-block btn-stacked'", 4]]],
      ['.token-display.hash-line', { 'font-size': '0.7rem', 'word-break': 'break-all', 'margin-top': '0.25rem' },
        [["className: 'token-display hash-line'", 3]]],
      ['.section-gap', { 'margin-top': '1rem' }, [["className: 'card-title mb-8 section-gap'", 1]]],
      ['.empty-state .events-empty-msg', { padding: '20px 0' },
        [["el('div', { className: 'empty-state' }, [ el('p', { className: 'events-empty-msg'", 1]]],
      ['.event-detail-subevent .subevent-time',
        { 'margin-left': 'auto', color: 'var(--color-text-muted)', 'font-size': '11px' },
        [["var subRow = el('div', { className: 'event-detail-subevent' });", 1],
          ["subRow.appendChild(el('span', { className: 'subevent-time'", 1]]],
      ['.card-subtitle.settings-heading', { 'font-weight': '500', color: 'var(--color-text)' },
        [["className: 'card-subtitle settings-heading mt-12 mb-8'", 2]]],
      ['.form-group.quiet-hours-row', { display: 'flex', gap: '12px', 'margin-top': '8px' },
        [["el('div', { className: 'form-group quiet-hours-row' })", 1]]],
      ['.flex-1', { flex: '1' }, [["el('div', { className: 'flex-1' })", 2]]],
      ['.fleet-stat-chip.rssi-good', { color: 'var(--color-success)' }, [["rssiVal > -50 ? 'rssi-good'", 1]]],
      ['.fleet-stat-chip.rssi-fair', { color: 'var(--color-warning)' }, [["rssiVal > -70 ? 'rssi-fair'", 1]]],
      ['.fleet-stat-chip.rssi-poor', { color: 'var(--color-error)' }, [[": 'rssi-poor';", 1]]],
    ];

    // styles.css's top-level rules: selector list and declarations, with
    // @media, @keyframes and their contents left out.
    const css = fs.readFileSync(path.join(SPA_DIR, 'styles.css'), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
    const rules = [];
    for (let i = 0; i < css.length;) {
      const open = css.indexOf('{', i);
      if (open < 0) break;
      let close = open + 1;
      for (let depth = 1; depth; close++) {
        assert.ok(close < css.length, 'styles.css: an unclosed block');
        if (css[close] === '{') depth++;
        else if (css[close] === '}') depth--;
      }
      const prelude = css.slice(i, open).trim();
      if (!prelude.startsWith('@')) {
        rules.push({
          selectors: prelude.split(',').map((s) => s.trim().replace(/\s+/g, ' ')),
          decls: Object.fromEntries(css.slice(open + 1, close - 1).split(';').map((d) => d.trim()).filter(Boolean)
            .map((d) => [d.slice(0, d.indexOf(':')).trim(), d.slice(d.indexOf(':') + 1).trim()])),
        });
      }
      i = close;
    }
    const flat = fs.readFileSync(path.join(SPA_DIR, 'app.js'), 'utf8').replace(/\s+/g, ' ');
    const count = (needle) => flat.split(needle.replace(/\s+/g, ' ')).length - 1;

    for (const [selector, decls, uses] of LAYOUT) {
      const declared = rules.filter((r) => r.selectors.includes(selector));
      assert.equal(declared.length, 1, selector + ': ' + declared.length + ' top-level rules in styles.css');
      assert.deepEqual(declared[0].decls, decls, selector + ' carries other declarations than the inline style did');
      for (const [fragment, times] of uses) {
        assert.equal(count(fragment), times, selector + ': app.js has `' + fragment + '` ' + count(fragment) +
          ' time(s), not ' + times);
      }
    }
    assert.equal(count("className: 'fleet-stat-chip ' + rssiClass"), 1, 'the RSSI chip lost fleet-stat-chip');
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
