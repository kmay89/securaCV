/*
 * Tests for securacv-strategy.js — the SecuraCV dashboard strategy.
 * Run: node --test custom_components/securacv/www/securacv-strategy.test.js
 *
 * Two layers, same dual-use convention as the cards:
 *   1. The file is import-evaluated in a sandbox with just enough browser
 *      surface to register. Home Assistant loads every SecuraCV module through
 *      one `add_extra_js_url` path, so a syntax error here would blank the
 *      cards too — the first test is the one that catches that.
 *   2. `require()` yields the pure helpers; `generate()` is driven with a fake
 *      hass (states + entity registry + device registry) through the states a
 *      Frigate user actually meets: connected kernel with cameras, quiet night,
 *      unclaimed bridge, Canaries only, nothing at all.
 */
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const S = require("./securacv-strategy.js");

const SRC = path.join(__dirname, "securacv-strategy.js");

function evaluateInBrowserSandbox() {
  const src = fs.readFileSync(SRC, "utf8");
  const defined = {};
  class FakeHTMLElement {}
  const sandbox = {
    HTMLElement: FakeHTMLElement,
    customElements: { define: (name, cls) => { defined[name] = cls; }, get: (name) => defined[name] },
    console: { debug() {}, log() {}, warn() {}, error() {} },
  };
  sandbox.window = sandbox;
  vm.runInNewContext(src, sandbox, { filename: "securacv-strategy.js" });
  return { sandbox, defined };
}

test("the module evaluates in a browser-like sandbox and registers the strategy", async () => {
  const { sandbox, defined } = evaluateInBrowserSandbox();
  const cls = defined["ll-strategy-dashboard-securacv"];
  assert.ok(cls, "ll-strategy-dashboard-securacv is defined");
  assert.equal(typeof cls.generate, "function");
  assert.equal(cls.noEditor, true);
  // (made in the sandbox realm — compare fields, not identity)
  const suggestions = cls.getCreateSuggestions();
  assert.equal(suggestions.title, "SecuraCV");
  assert.equal(suggestions.icon, "mdi:shield-check");
  // Listed under Settings → Dashboards → Add dashboard → Community dashboards.
  assert.equal(sandbox.customStrategies.length, 1);
  const entry = sandbox.customStrategies[0];
  assert.equal(entry.type, "securacv");
  assert.equal(entry.strategyType, "dashboard");
  assert.equal(entry.name, "SecuraCV");
  assert.match(entry.documentationURL, /^https:\/\//);
  // The element's generate is the same generator the tests below drive.
  const dash = await cls.generate({}, { states: {}, entities: {}, devices: {}, config: { components: [] } });
  assert.equal(dash.title, "SecuraCV");
  assert.equal(dash.views[0].type, "sections");
});

test("evaluating the module twice does not double-register", () => {
  const src = fs.readFileSync(SRC, "utf8");
  const defined = {};
  const sandbox = {
    HTMLElement: class {},
    customElements: { define: (name, cls) => { if (defined[name]) throw new Error("already defined"); defined[name] = cls; }, get: (name) => defined[name] },
    console: { debug() {}, log() {}, warn() {}, error() {} },
  };
  sandbox.window = sandbox;
  vm.runInNewContext(src, sandbox, { filename: "securacv-strategy.js" });
  vm.runInNewContext(src, sandbox, { filename: "securacv-strategy.js" });
  assert.equal(sandbox.customStrategies.length, 1);
});

// --- a fake Home Assistant ---------------------------------------------------

const KERNEL_DEVICE = "dev_kernel";
const CANARY_DEVICE = "dev_canary_ab7k";
const MQTT_CANARY_DEVICE = "dev_mqtt_ab7k";

function reg(entityId, translationKey, deviceId, extra) {
  return Object.assign({ entity_id: entityId, platform: "securacv", translation_key: translationKey, device_id: deviceId }, extra || {});
}

/** A connected kernel with two cameras (one with a Frigate picture), a Canary, and unrelated entities. */
function connectedHass(overrides) {
  const bucket = { start_epoch_s: 1760140800, size_s: 600 };
  const witnessAttrs = (zone) => ({
    event_type: "boundary_crossing_object_large", friendly_event: "Person or vehicle",
    zone_id: `zone:${zone}`, zone, time_bucket: bucket, confidence: 0.87, kernel_device_id: "pwk_1a2b3c4d",
    verification: "sealed", verified: false, trust_reason: "kernel_sealed", seq: 12,
    event_types: ["boundary_crossing_object_large", "boundary_crossing_object_small"],
    friendly_name: `SecuraCV ${zone === "front_door" ? "Front door" : "Driveway"}`,
  });
  const hass = {
    config: { components: ["hassio", "frigate", "securacv", "mqtt"] },
    states: {
      "binary_sensor.securacv_chain_valid": { state: "on", attributes: { verified_bucket_start: 1760140800, verified_entries: 1204, friendly_name: "SecuraCV Chain valid" } },
      "sensor.securacv_chain_length": { state: "1204", attributes: { latest_hash: "9c1e", pinned: true } },
      "sensor.securacv_daily_digest": { state: "7", attributes: { per_zone: { "zone:front_door": 4, "zone:driveway": 3 }, per_period: { morning: 2, evening: 5 }, window_secs: 86400 } },
      "sensor.securacv_last_event": { state: "boundary_crossing_object_large", attributes: witnessAttrs("front_door") },
      "button.securacv_verify_now": { state: "unknown", attributes: {} },
      "button.securacv_export_evidence": { state: "unknown", attributes: {} },
      "event.securacv_verification_events": { state: "unknown", attributes: { event_types: ["chain_verified", "chain_verification_failed", "witness_key_changed"] } },
      "binary_sensor.securacv_kernel_online": { state: "on", attributes: {} },
      "binary_sensor.securacv_frigate_link": { state: "on", attributes: { last_message_bucket_start: 1760140800, topic_prefix: "frigate", mode: "events" } },
      "sensor.securacv_kernel_fingerprint": { state: "ab12cd34", attributes: {} },
      "sensor.securacv_storage_free": { state: "81", attributes: {} },
      "event.securacv_front_door": { state: "2026-10-11T14:03:00+00:00", attributes: witnessAttrs("front_door") },
      "event.securacv_driveway": { state: "unknown", attributes: { event_types: ["boundary_crossing_object_large"], friendly_name: "SecuraCV Driveway" } },
      // Frigate's own cameras: one matches a witness entity, one is somebody else's.
      "camera.front_door": { state: "streaming", attributes: {} },
      "camera.driveway": { state: "streaming", attributes: {} },
      // The integration's Canary device and the firmware's own MQTT device.
      "binary_sensor.securacv_canary_ab7k_online": { state: "on", attributes: {} },
      "sensor.securacv_canary_ab7k_last_event": { state: "contact_state_change", attributes: { zone_id: "zone:door", confidence: 0.9, verified: true, trust_reason: "pinned", time_bucket: bucket } },
      "binary_sensor.securacv_canary_ab7k_chain_valid": { state: "on", attributes: {} },
      "sensor.canary_ab7k_health": { state: "healthy", attributes: {} },
      "sensor.canary_ab7k_rssi": { state: "-61", attributes: {} },
      // Noise.
      "sensor.living_room_temperature": { state: "21", attributes: {} },
      "binary_sensor.front_door_contact": { state: "off", attributes: {} },
    },
    entities: {
      "binary_sensor.securacv_chain_valid": reg("binary_sensor.securacv_chain_valid", "chain_valid", KERNEL_DEVICE),
      "sensor.securacv_chain_length": reg("sensor.securacv_chain_length", "chain_length", KERNEL_DEVICE),
      "sensor.securacv_daily_digest": reg("sensor.securacv_daily_digest", "daily_digest", KERNEL_DEVICE),
      "sensor.securacv_last_event": reg("sensor.securacv_last_event", "kernel_last_event", KERNEL_DEVICE),
      "button.securacv_verify_now": reg("button.securacv_verify_now", "verify_now", KERNEL_DEVICE),
      "button.securacv_export_evidence": reg("button.securacv_export_evidence", "export_evidence", KERNEL_DEVICE),
      "event.securacv_verification_events": reg("event.securacv_verification_events", "verification_events", KERNEL_DEVICE),
      "binary_sensor.securacv_kernel_online": reg("binary_sensor.securacv_kernel_online", "kernel_online", KERNEL_DEVICE, { entity_category: "diagnostic" }),
      "binary_sensor.securacv_frigate_link": reg("binary_sensor.securacv_frigate_link", "frigate_link", KERNEL_DEVICE, { entity_category: "diagnostic" }),
      "sensor.securacv_kernel_fingerprint": reg("sensor.securacv_kernel_fingerprint", "kernel_fingerprint", KERNEL_DEVICE, { entity_category: "diagnostic" }),
      "sensor.securacv_storage_free": reg("sensor.securacv_storage_free", "storage_free_pct", KERNEL_DEVICE, { entity_category: "diagnostic" }),
      "event.securacv_front_door": reg("event.securacv_front_door", "witness", KERNEL_DEVICE),
      "event.securacv_driveway": reg("event.securacv_driveway", "witness", KERNEL_DEVICE),
      "camera.front_door": { entity_id: "camera.front_door", platform: "frigate", device_id: "dev_frigate", translation_key: null },
      "camera.driveway": { entity_id: "camera.driveway", platform: "generic", device_id: "dev_other", translation_key: null },
      "binary_sensor.securacv_canary_ab7k_online": reg("binary_sensor.securacv_canary_ab7k_online", "online", CANARY_DEVICE),
      "sensor.securacv_canary_ab7k_last_event": reg("sensor.securacv_canary_ab7k_last_event", "last_event", CANARY_DEVICE),
      "binary_sensor.securacv_canary_ab7k_chain_valid": reg("binary_sensor.securacv_canary_ab7k_chain_valid", "chain_valid", CANARY_DEVICE),
      "sensor.canary_ab7k_health": { entity_id: "sensor.canary_ab7k_health", platform: "mqtt", device_id: MQTT_CANARY_DEVICE, translation_key: null },
      "sensor.canary_ab7k_rssi": { entity_id: "sensor.canary_ab7k_rssi", platform: "mqtt", device_id: MQTT_CANARY_DEVICE, translation_key: null, entity_category: "diagnostic" },
    },
    devices: {
      [KERNEL_DEVICE]: { id: KERNEL_DEVICE, name: "SecuraCV", identifiers: [["securacv", "kernel_pwk_1a2b3c4d"], ["securacv", "http://x:8799"]] },
      [CANARY_DEVICE]: { id: CANARY_DEVICE, name: "SecuraCV Canary ab7k", identifiers: [["securacv", "canary_ab7k"]] },
      [MQTT_CANARY_DEVICE]: { id: MQTT_CANARY_DEVICE, name: "SecuraCV Canary Sense ab7k", identifiers: [["mqtt", "securacv_ab7k"]] },
      dev_frigate: { id: "dev_frigate", name: "Frigate", identifiers: [["frigate", "frigate"]] },
      dev_other: { id: "dev_other", name: "Other", identifiers: [["generic", "x"]] },
    },
  };
  return Object.assign(hass, overrides || {});
}

const headingsOf = (dash) => dash.views[0].sections.map((s) => s.cards[0].heading);
const cardsOfType = (section, type) => section.cards.filter((c) => c.type === type);
const flatten = (cards) => cards.flatMap((c) => (c.type === "vertical-stack" ? flatten(c.cards) : [c]));

test("collectModel: the registry's translation keys decide roles; Canaries join their MQTT device by id", () => {
  const model = S.collectModel(connectedHass());
  assert.equal(model.hasRegistry, true);
  assert.equal(model.kernels.length, 1);
  const k = model.kernels[0];
  assert.equal(k.deviceId, KERNEL_DEVICE);
  assert.equal(k.entity.chainValid, "binary_sensor.securacv_chain_valid");
  assert.equal(k.entity.chainLength, "sensor.securacv_chain_length");
  assert.equal(k.entity.dailyDigest, "sensor.securacv_daily_digest");
  assert.equal(k.entity.lastEvent, "sensor.securacv_last_event");
  assert.equal(k.entity.verifyNow, "button.securacv_verify_now");
  assert.equal(k.entity.exportEvidence, "button.securacv_export_evidence");
  assert.equal(k.entity.frigateLink, "binary_sensor.securacv_frigate_link");
  assert.equal(k.entity.kernelOnline, "binary_sensor.securacv_kernel_online");
  assert.equal(k.entity.fingerprint, "sensor.securacv_kernel_fingerprint");
  assert.deepEqual(k.storage, ["sensor.securacv_storage_free"]);
  // Witness entities sorted by name, zone from the attributes (or the id), the
  // Frigate camera only when Frigate owns `camera.<slug>`.
  assert.deepEqual(k.witness.map((w) => [w.entityId, w.zone, w.name, w.camera]), [
    ["event.securacv_driveway", "driveway", "Driveway", null],
    ["event.securacv_front_door", "front_door", "Front door", "camera.front_door"],
  ]);
  // One Canary group: the integration's device + the firmware's MQTT device.
  assert.equal(model.canaries.length, 1);
  const c = model.canaries[0];
  assert.equal(c.id, "ab7k");
  assert.deepEqual(c.deviceIds.sort(), [CANARY_DEVICE, MQTT_CANARY_DEVICE].sort());
  assert.deepEqual(c.entities, [
    "binary_sensor.securacv_canary_ab7k_chain_valid",
    "binary_sensor.securacv_canary_ab7k_online",
    "sensor.securacv_canary_ab7k_last_event",
  ]);
  // Diagnostic MQTT entities stay off the dashboard; the device's own readings are on it.
  assert.deepEqual(c.mqttEntities, ["sensor.canary_ab7k_health"]);
  assert.equal(model.bridge, null, "a claimed kernel means no bridge section");
});

test("canaryIdOf reads both identifier spellings and ignores strangers", () => {
  assert.equal(S.canaryIdOf({ identifiers: [["securacv", "canary_ab7k"]] }), "ab7k");
  assert.equal(S.canaryIdOf({ identifiers: [["mqtt", "securacv_ab7k"]] }), "ab7k");
  assert.equal(S.canaryIdOf({ identifiers: [["mqtt", "securacv_canary_ab7k"]] }), "ab7k");
  assert.equal(S.canaryIdOf({ identifiers: [["zigbee", "00:11"]] }), null);
  assert.equal(S.canaryIdOf({}), null);
});

test("generate: a connected kernel gets the six sections in order, with honest copy", async () => {
  const dash = await S.generate({}, connectedHass());
  assert.equal(dash.title, "SecuraCV");
  const view = dash.views[0];
  assert.equal(view.type, "sections");
  assert.deepEqual(headingsOf(dash), ["SecuraCV", "Today", "Cameras", "Last 24 hours", "Health", "Canaries"]);

  // Header: the chain line is the INTEGRATION's walk, the buttons press the
  // integration's buttons, the coarsening is explained once.
  const header = view.sections[0];
  const md = cardsOfType(header, "markdown").map((c) => c.content).join("\n");
  assert.match(md, /Chain verified ✓ \{\{ relative_time\(as_datetime\(vbs\)\) \}\} ago/);
  assert.match(md, /key pinned/);
  assert.match(md, /Verifying…/);
  assert.match(md, /Verification failed/);
  assert.match(md, /Not verified yet/);
  assert.match(md, /Witness key changed — confirm it in Settings → Repairs/);
  // A key change is decided on the chain_valid entity's own `key_mismatch`
  // attribute (the sensor sets it, with `trust_reason: kernel_key_mismatch`
  // and an `error` naming Repairs), and that branch comes BEFORE the
  // `st == 'off'` branch, so a key change never renders as a failure.
  assert.match(md, /state_attr\(cv, 'key_mismatch'\) is true/);
  assert.match(md, /state_attr\(cv, 'trust_reason'\) == 'kernel_key_mismatch'/);
  const keyChangedAt = md.indexOf("Witness key changed");
  const failedAt = md.indexOf("Verification failed");
  const offBranchAt = md.indexOf("st == 'off'");
  assert.ok(keyChangedAt > -1 && failedAt > -1 && offBranchAt > -1);
  assert.ok(keyChangedAt < offBranchAt && offBranchAt < failedAt, "the key-change branch precedes the off branch");
  assert.match(md, /SecuraCV can't reach the witness kernel/);
  assert.match(md, /Frigate not heard/);
  assert.match(md, /SecuraCV records coarse claims/);
  assert.match(md, /One sealed event per camera, label and 10-minute window/);
  assert.match(md, /binary_sensor\.securacv_chain_valid/);
  const tiles = cardsOfType(header, "tile");
  assert.deepEqual(tiles.map((t) => [t.entity, t.name]), [
    ["button.securacv_verify_now", "Verify now"],
    ["button.securacv_export_evidence", "Export evidence"],
  ]);
  for (const t of tiles) {
    assert.equal(t.tap_action.action, "perform-action");
    assert.equal(t.tap_action.perform_action, "button.press");
    assert.equal(t.tap_action.target.entity_id, t.entity);
  }

  // Today: the timeline card bound to the witness entities + the last-event sensor.
  const today = view.sections[1];
  const timeline = cardsOfType(today, "custom:securacv-timeline-card")[0];
  assert.ok(timeline);
  assert.deepEqual(timeline.event_entities, ["event.securacv_driveway", "event.securacv_front_door", "sensor.securacv_last_event"]);
  assert.equal(timeline.chain_valid_entity, "binary_sensor.securacv_chain_valid");
  assert.equal(timeline.chain_length_entity, "sensor.securacv_chain_length");
  assert.equal(timeline.hours, 24);
  assert.equal(cardsOfType(today, "markdown").length, 0, "no empty-state line while cameras exist");

  // Cameras: one stack per witness entity; Frigate's picture only where Frigate owns the camera.
  const cameras = view.sections[2];
  const stacks = cardsOfType(cameras, "vertical-stack");
  assert.equal(stacks.length, 2);
  const driveway = flatten([stacks[0]]);
  const frontDoor = flatten([stacks[1]]);
  assert.equal(driveway.filter((c) => c.type === "picture-entity").length, 0, "camera.driveway is not Frigate's");
  const pic = frontDoor.find((c) => c.type === "picture-entity");
  assert.equal(pic.entity, "camera.front_door");
  const frontTile = frontDoor.find((c) => c.type === "tile");
  assert.equal(frontTile.entity, "event.securacv_front_door");
  assert.equal(frontTile.name, "Front door");
  assert.equal(frontTile.tap_action.action, "more-info");
  const frontMd = frontDoor.find((c) => c.type === "markdown").content;
  assert.match(frontMd, /Verified ✓/);
  assert.match(frontMd, /Sealed ✓/);
  assert.match(frontMd, /Failed ⚠/);
  assert.match(frontMd, /'Person or vehicle'/);
  assert.match(frontMd, /in the last 24 h/);
  assert.match(frontMd, /'zone:front_door'/);
  // Open Frigate defaults to the app's ingress path on Home Assistant OS and
  // never promises a clip.
  const open = frontDoor.find((c) => c.type === "button");
  assert.equal(open.name, "Open Frigate");
  assert.deepEqual(open.tap_action, { action: "navigate", navigation_path: "/hassio/ingress/ccab4aaf_frigate" });
  assert.doesNotMatch(JSON.stringify(cameras), /clip/i);

  // Last 24 hours: the digest by period and camera, periods named honestly.
  const digest = view.sections[3];
  const digestMd = cardsOfType(digest, "markdown")[0].content;
  assert.match(digestMd, /sensor\.securacv_daily_digest/);
  assert.match(digestMd, /per_period/);
  assert.match(digestMd, /per_zone/);
  assert.match(digestMd, /kernel-reported/);
  assert.match(digestMd, /UTC quarters/);

  // Health: the diagnostics, by entity.
  const health = view.sections[4];
  const rows = cardsOfType(health, "entities")[0].entities.map((r) => r.entity);
  assert.deepEqual(rows, [
    "binary_sensor.securacv_frigate_link",
    "binary_sensor.securacv_kernel_online",
    "sensor.securacv_chain_length",
    "sensor.securacv_kernel_fingerprint",
    "sensor.securacv_storage_free",
  ]);

  // Canaries: one card per group, the integration's entities then the device's own.
  const canaries = view.sections[5];
  const card = cardsOfType(canaries, "entities")[0];
  assert.equal(card.title, "SecuraCV Canary ab7k");
  assert.deepEqual(card.entities.map((r) => r.entity || r.label), [
    "binary_sensor.securacv_canary_ab7k_chain_valid",
    "binary_sensor.securacv_canary_ab7k_online",
    "sensor.securacv_canary_ab7k_last_event",
    "Reported by the device",
    "sensor.canary_ab7k_health",
  ]);

  // Nothing in the dashboard names an entity SecuraCV does not own or Frigate's pictures.
  const text = JSON.stringify(dash);
  assert.doesNotMatch(text, /living_room_temperature|front_door_contact/);
});

test("generate: frigate_url and hours options are honored; no link without a destination", async () => {
  const external = await S.generate({ frigate_url: "https://frigate.local:8971/", hours: 6 }, connectedHass());
  const cameras = external.views[0].sections[2];
  const open = flatten(cameras.cards).find((c) => c.type === "button");
  assert.deepEqual(open.tap_action, { action: "url", url_path: "https://frigate.local:8971/" });
  assert.equal(cardsOfType(external.views[0].sections[1], "custom:securacv-timeline-card")[0].hours, 6);

  // Not on Home Assistant OS and nothing configured: no dead link.
  const container = await S.generate({}, connectedHass({ config: { components: ["frigate", "securacv"] } }));
  assert.equal(flatten(container.views[0].sections[2].cards).filter((c) => c.type === "button").length, 0);
  assert.equal(S.resolveFrigateUrl({}, { config: { components: ["hassio"] } }), S.DEFAULT_FRIGATE_INGRESS);
  assert.equal(S.resolveFrigateUrl({ frigate_url: "  " }, { config: { components: [] } }), null);
});

test("generate: a quiet kernel with no witness entity yet says so instead of showing nothing", async () => {
  const hass = connectedHass();
  for (const eid of ["event.securacv_front_door", "event.securacv_driveway"]) {
    delete hass.states[eid];
    delete hass.entities[eid];
  }
  const dash = await S.generate({}, hass);
  assert.deepEqual(headingsOf(dash), ["SecuraCV", "Today", "Last 24 hours", "Health", "Canaries"], "no Cameras section without cameras");
  const today = dash.views[0].sections[1];
  const empty = cardsOfType(today, "markdown")[0].content;
  assert.match(empty, /No witness events yet\./);
  assert.match(empty, /Frigate heard \{\{ relative_time\(as_datetime\(heard\)\) \}\} ago ✓/);
  assert.match(empty, /the next person, car, dog or cat Frigate reports appears here sealed/);
  assert.match(empty, /Frigate has not been heard/);
  // The timeline card still sits below, bound to the last-event sensor.
  const timeline = cardsOfType(today, "custom:securacv-timeline-card")[0];
  assert.deepEqual(timeline.event_entities, ["sensor.securacv_last_event"]);
});

test("generate: without a registry the integration's generated ids are the fallback", async () => {
  const hass = connectedHass();
  delete hass.entities;
  delete hass.devices;
  const model = S.collectModel(hass);
  assert.equal(model.hasRegistry, false);
  assert.equal(model.kernels.length, 1);
  const k = model.kernels[0];
  assert.equal(k.entity.chainValid, "binary_sensor.securacv_chain_valid");
  assert.equal(k.entity.exportEvidence, "button.securacv_export_evidence");
  assert.deepEqual(k.witness.map((w) => w.entityId), ["event.securacv_driveway", "event.securacv_front_door"]);
  // Without a registry a camera by the zone's name is the best available match.
  assert.equal(k.witness[1].camera, "camera.front_door");
  assert.equal(model.canaries.length, 1);
  assert.equal(model.canaries[0].id, "ab7k");
  const dash = await S.generate({}, hass);
  assert.deepEqual(headingsOf(dash), ["SecuraCV", "Today", "Cameras", "Last 24 hours", "Health", "Canaries"]);
});

test("generate: an unclaimed bridge is shown as kernel-reported, never verified", async () => {
  const hass = {
    config: { components: ["hassio", "mqtt"] },
    states: {
      "sensor.pwk_last_event": { state: "BoundaryCrossingObjectLarge", attributes: { zone_id: "zone:front_door", confidence: 0.9, time_bucket_start: 1760140800, time_bucket_size: 600 } },
      "sensor.pwk_daily_digest": { state: "3", attributes: { per_zone: { "zone:front_door": 3 } } },
      "binary_sensor.pwk_chain_problem": { state: "off", attributes: { device_class: "problem", chain_valid: true, events_verified: 42 } },
      "button.pwk_verify_now": { state: "unknown", attributes: {} },
      "sensor.pwk_front_door_events": { state: "3", attributes: {} },
      "binary_sensor.pwk_front_door_motion": { state: "off", attributes: {} },
    },
    entities: {},
    devices: {},
  };
  const model = S.collectModel(hass);
  assert.equal(model.kernels.length, 0);
  assert.ok(model.bridge);
  assert.equal(model.bridge.chainProblem, "binary_sensor.pwk_chain_problem");
  assert.deepEqual(model.bridge.zones, [{ zone: "front_door", count: "sensor.pwk_front_door_events", motion: "binary_sensor.pwk_front_door_motion" }]);

  const dash = await S.generate({}, hass);
  assert.deepEqual(headingsOf(dash), ["SecuraCV", "Today", "Cameras", "Last 24 hours", "Health"]);
  const header = dash.views[0].sections[0];
  const md = cardsOfType(header, "markdown").map((c) => c.content).join("\n");
  assert.match(md, /Chain OK \(kernel-reported\)/);
  assert.match(md, /Chain check failed \(kernel-reported\)/);
  assert.doesNotMatch(md, /Chain verified ✓/);
  assert.match(md, /Add the SecuraCV integration/);
  const timeline = cardsOfType(dash.views[0].sections[1], "custom:securacv-timeline-card")[0];
  assert.deepEqual(timeline.event_entities, ["sensor.pwk_last_event"]);
  assert.equal(timeline.chain_valid_entity, "binary_sensor.pwk_chain_problem");
  // No export button: there is no integration to serve the service.
  assert.doesNotMatch(JSON.stringify(dash), /export_evidence/);
});

test("generate: Canaries only — the kernel sections wait honestly", async () => {
  const hass = connectedHass();
  for (const eid of Object.keys(hass.entities)) {
    if (hass.entities[eid].device_id === KERNEL_DEVICE) {
      delete hass.entities[eid];
      delete hass.states[eid];
    }
  }
  const dash = await S.generate({}, hass);
  assert.deepEqual(headingsOf(dash), ["SecuraCV", "Today", "Canaries"]);
  const md = cardsOfType(dash.views[0].sections[0], "markdown")[0].content;
  assert.match(md, /No witness kernel is connected yet — only Canary devices/);
  // The timeline card auto-discovers Canary rows: no explicit entities.
  const timeline = cardsOfType(dash.views[0].sections[1], "custom:securacv-timeline-card")[0];
  assert.equal(timeline.event_entities, undefined);
});

test("generate: nothing at all → one card that says what to do", async () => {
  const dash = await S.generate({}, { states: { "sensor.x": { state: "1", attributes: {} } }, entities: {}, devices: {}, config: { components: [] } });
  assert.deepEqual(headingsOf(dash), ["SecuraCV"]);
  const md = cardsOfType(dash.views[0].sections[0], "markdown")[0].content;
  assert.match(md, /SecuraCV is not set up yet/);
  assert.match(md, /\/config\/integrations\/dashboard\/add\?domain=securacv/);
  // A hass with none of the registries at all is tolerated too.
  const bare = await S.generate(undefined, {});
  assert.deepEqual(headingsOf(bare), ["SecuraCV"]);
});

test("text helpers keep names and ids out of markup and templates", () => {
  assert.equal(S.slugify("Front Door"), "front_door");
  assert.equal(S.slugify("front-door"), "front_door");
  assert.equal(S.humanize("front_door"), "Front door");
  assert.equal(S.mdText("<b>x</b> {{ states('a') }} [y]"), "&lt;b&gt;x&lt;/b&gt; \\{\\{ states('a') \\}\\} \\[y\\]");
  assert.equal(S.jinjaStr("sensor.a'b{%c"), "'sensor.abc'");
});
