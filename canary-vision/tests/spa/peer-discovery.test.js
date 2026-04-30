'use strict';

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

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

  it('prefers mdns_hostname over ip when constructing peer base_url', () => {
    // mDNS names survive DHCP lease changes; IPs do not.
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
