/*
 * Unit tests for the pure data-shaping helpers in securacv-timeline-card.js.
 * Run: node --test custom_components/securacv/www/securacv-timeline-card.test.js
 *
 * The card file guards its custom-element registration on `customElements`, so
 * requiring it under Node yields just the helper surface — the same dual-use
 * pattern as viewer/verify_core.js. These tests cover the logic where bugs hide
 * (verification semantics, history de-dup, discovery) without needing a browser.
 */
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");

const {
  normalizeEventType,
  eventMeta,
  confidencePct,
  formatTimeBucket,
  resolveVerification,
  normalizeHistoryEntry,
  historyToTimelineItems,
  discoverEntities,
} = require("./securacv-timeline-card.js");

test("normalizeEventType accepts snake_case and CamelCase enum forms", () => {
  assert.equal(normalizeEventType("acoustic_impulse_in_zone"), "acoustic_impulse_in_zone");
  assert.equal(normalizeEventType("AcousticImpulseInZone"), "acoustic_impulse_in_zone");
  assert.equal(normalizeEventType("BoundaryCrossingObjectLarge"), "boundary_crossing_object_large");
  assert.equal(normalizeEventType(""), "");
});

test("eventMeta resolves known types and falls back for unknown", () => {
  assert.deepEqual(eventMeta("boundary_crossing_object_large"), {
    label: "Large object crossed boundary",
    icon: "mdi:car",
  });
  // CamelCase resolves to the same metadata.
  assert.equal(eventMeta("ContactStateChange").icon, "mdi:door");
  // Unknown type keeps the raw label but uses the default icon.
  const unknown = eventMeta("some_future_claim");
  assert.equal(unknown.icon, "mdi:shield-eye");
  assert.equal(unknown.label, "some_future_claim");
  assert.equal(eventMeta(null).label, "Unknown");
});

test("confidencePct clamps, rounds, and rejects junk", () => {
  assert.equal(confidencePct(0.85), 85);
  assert.equal(confidencePct("0.5"), 50);
  assert.equal(confidencePct(1.4), 100);
  assert.equal(confidencePct(-0.2), 0);
  assert.equal(confidencePct("nope"), null);
  assert.equal(confidencePct(undefined), null);
});

test("formatTimeBucket renders coarse windows, strings, and empties", () => {
  // 1970-01-01 00:10:00 UTC start; rendered in local time, so assert structure.
  const win = formatTimeBucket({ start_epoch_s: 600, size_s: 600 });
  assert.match(win, /^\d{2}:\d{2}–\d{2}:\d{2} \(~10 min\)$/);
  assert.equal(formatTimeBucket("yesterday morning"), "yesterday morning");
  assert.equal(formatTimeBucket(null), "");
  assert.equal(formatTimeBucket({ size_s: 600 }), "");
});

test("resolveVerification only awards ✓ verified on a real check", () => {
  assert.equal(resolveVerification({ verified: true }).level, "verified");
  assert.equal(resolveVerification({ verified: true }).symbol, "✓");

  // signed-but-not-verified is a distinct, weaker badge.
  const signed = resolveVerification({ signed: true });
  assert.equal(signed.level, "signed");
  assert.equal(signed.symbol, "✓");
  assert.equal(signed.label, "Signed (unverified)");

  // a real failed check / trust mismatch surfaces a warning.
  const failed = resolveVerification({ verified: false, trustReason: "fingerprint_mismatch" });
  assert.equal(failed.level, "failed");
  assert.equal(failed.symbol, "⚠");

  // verified:false with only "no_pubkey" is not a failure — it's just unproven.
  assert.equal(resolveVerification({ verified: false, trustReason: "no_pubkey" }).level, "logged");

  // kernel HTTP path with no signals → neutral "logged", never a green check.
  const logged = resolveVerification({});
  assert.equal(logged.level, "logged");
  assert.equal(logged.symbol, "·");
});

test("normalizeHistoryEntry handles compact and verbose shapes", () => {
  const compact = normalizeHistoryEntry({ s: "ContactStateChange", a: { zone: "zone:a" }, lu: 1700000000.5 });
  assert.equal(compact.state, "ContactStateChange");
  assert.equal(compact.attributes.zone, "zone:a");
  assert.equal(compact.ts, 1700000000500);

  const verbose = normalizeHistoryEntry({
    state: "boundary_crossing_object_large",
    attributes: { confidence: 0.9 },
    last_changed: "2026-06-04T12:00:00Z",
  });
  assert.equal(verbose.state, "boundary_crossing_object_large");
  assert.equal(verbose.ts, Date.parse("2026-06-04T12:00:00Z"));
  assert.equal(normalizeHistoryEntry(null), null);
});

test("historyToTimelineItems carries attributes forward, drops unavailable, de-dups", () => {
  const history = {
    "sensor.securacv_last_event": [
      // attributes present on first sample, then omitted (compact recorder shape)
      { s: "boundary_crossing_object_large", a: { zone_id: "zone:a", confidence: 0.9, time_bucket: { start_epoch_s: 600, size_s: 600 }, verified: true, trust_reason: "pinned" }, lu: 100 },
      // same state + bucket → collapsed
      { s: "boundary_crossing_object_large", lu: 160 },
      // recorder gap
      { s: "unavailable", lu: 200 },
      // new event, attributes carried forward except the ones that change
      { s: "contact_state_change", a: { zone_id: "zone:b", confidence: 0.7, signed: true, time_bucket: { start_epoch_s: 1200, size_s: 600 } }, lu: 300 },
    ],
  };
  const items = historyToTimelineItems(history, { maxEvents: 50 });
  assert.equal(items.length, 2, "two distinct events after de-dup + gap drop");

  // newest first
  assert.equal(items[0].eventType, "contact_state_change");
  assert.equal(items[0].zone, "zone:b");
  assert.equal(items[0].confidence, 70);
  assert.equal(items[0].verification.level, "signed");

  assert.equal(items[1].eventType, "boundary_crossing_object_large");
  assert.equal(items[1].verification.level, "verified");
  assert.equal(items[1].confidence, 90);
  assert.match(items[1].timeBucket, /~10 min/);
});

test("historyToTimelineItems keeps same-type events in different zones (zone in de-dup key)", () => {
  const bucket = { start_epoch_s: 600, size_s: 600 }; // same coarse 10-min window
  const history = {
    "sensor.securacv_last_event": [
      { s: "boundary_crossing_object_small", a: { zone_id: "zone:driveway", time_bucket: bucket }, lu: 100 },
      // same type + same bucket but a DIFFERENT zone → must NOT be collapsed
      { s: "boundary_crossing_object_small", a: { zone_id: "zone:garden", time_bucket: bucket }, lu: 160 },
      // exact repeat of the previous (same type/zone/bucket) → collapsed
      { s: "boundary_crossing_object_small", a: { zone_id: "zone:garden", time_bucket: bucket }, lu: 200 },
    ],
  };
  const items = historyToTimelineItems(history, { maxEvents: 50 });
  assert.equal(items.length, 2, "distinct zones kept, exact repeat collapsed");
  assert.deepEqual(items.map((i) => i.zone).sort(), ["zone:driveway", "zone:garden"]);
});

test("historyToTimelineItems respects maxEvents and tolerates junk", () => {
  const series = [];
  for (let i = 0; i < 10; i++) {
    series.push({ s: `t${i}`, a: { zone_id: "z" }, lu: i });
  }
  const items = historyToTimelineItems({ "sensor.x": series }, { maxEvents: 3 });
  assert.equal(items.length, 3);
  assert.deepEqual(historyToTimelineItems(null, {}), []);
  assert.deepEqual(historyToTimelineItems({ "sensor.x": "not-an-array" }, {}), []);
});

test("discoverEntities matches SecuraCV attribute signatures without false positives", () => {
  const states = {
    "sensor.securacv_last_event": { state: "contact_state_change", attributes: { friendly_event: "Contact state change", zone_id: "zone:a", confidence: 0.8 } },
    "sensor.securacv_canary_abc_chain_length": { state: "42", attributes: { latest_hash: "deadbeefcafe", algorithm: "ed25519" } },
    "binary_sensor.securacv_canary_abc_chain_valid": { state: "on", attributes: { friendly_name: "SecuraCV Canary abc Chain Valid" } },
    "binary_sensor.securacv_canary_abc_tamper": { state: "off", attributes: { friendly_name: "SecuraCV Canary abc Tamper" } },
    // unrelated entities must be ignored
    "sensor.living_room_temperature": { state: "21", attributes: { unit_of_measurement: "°C" } },
    "binary_sensor.front_door": { state: "off", attributes: { friendly_name: "Front Door" } },
  };
  const found = discoverEntities(states);
  assert.deepEqual(found.eventEntities, ["sensor.securacv_last_event"]);
  assert.equal(found.chainLengthEntity, "sensor.securacv_canary_abc_chain_length");
  assert.equal(found.chainValidEntity, "binary_sensor.securacv_canary_abc_chain_valid");
  assert.equal(found.tamperEntity, "binary_sensor.securacv_canary_abc_tamper");
});
