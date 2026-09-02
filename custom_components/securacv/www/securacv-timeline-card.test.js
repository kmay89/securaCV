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
  normalizeModality,
  resolveModality,
  resolveAttestation,
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

test("normalizeModality maps known values and aliases, rejects junk", () => {
  assert.equal(normalizeModality("radar"), "radar");
  assert.equal(normalizeModality("wifi-csi"), "wifi-csi");
  assert.equal(normalizeModality("wifi_csi"), "wifi-csi"); // underscore form
  assert.equal(normalizeModality("CSI"), "wifi-csi");      // alias + case
  assert.equal(normalizeModality("mmwave"), "radar");      // alias
  assert.equal(normalizeModality("camera"), "camera");
  assert.equal(normalizeModality("contact"), "contact");
  assert.equal(normalizeModality("teleportation"), null);  // unknown → no glyph
  assert.equal(normalizeModality(""), null);
  assert.equal(normalizeModality(undefined), null);
});

test("resolveModality prefers explicit modality, falls back to device_type", () => {
  // explicit modality wins
  assert.deepEqual(resolveModality({ modality: "radar" }), {
    key: "radar", label: "Radar", icon: "mdi:radar",
  });
  // device_type fallback maps canary-sense → radar
  assert.equal(resolveModality({ device_type: "canary-sense" }).key, "radar");
  assert.equal(resolveModality({ device_type: "canary-vision" }).key, "camera");
  // explicit modality takes precedence over a conflicting device_type
  assert.equal(resolveModality({ modality: "camera", device_type: "canary-sense" }).key, "camera");
  // nothing resolvable → null (backward-compatible "no indicator")
  assert.equal(resolveModality({}), null);
  assert.equal(resolveModality({ device_type: "unknown-thing" }), null);
  assert.equal(resolveModality(null), null);
});

test("resolveAttestation only chips non-device provenance", () => {
  // device-attested (the default) renders no chip
  assert.equal(resolveAttestation({ attestation: "device" }), null);
  assert.equal(resolveAttestation({}), null);
  // Track B provenance surfaces a distinct chip
  assert.deepEqual(resolveAttestation({ attestation: "adapter" }), {
    key: "adapter", label: "Adapter-attested", icon: "mdi:hub",
  });
  assert.equal(resolveAttestation({ attestation: "ha-bridged" }).label, "HA-bridged");
  assert.equal(resolveAttestation({ attestation: "ha_bridged" }).key, "ha-bridged"); // underscore form
  // junk → no chip (never invent provenance)
  assert.equal(resolveAttestation({ attestation: "made-up" }), null);
});

test("historyToTimelineItems attaches modality + attestation, omits when absent", () => {
  const bucket = { start_epoch_s: 600, size_s: 600 };
  const history = {
    "sensor.securacv_last_event": [
      // radar event, Track B adapter-attested
      { s: "presence_in_restricted_zone", a: { zone_id: "zone:closet", modality: "radar", attestation: "adapter", time_bucket: bucket }, lu: 300 },
      // legacy event with no modality/attestation → both null (renders as before)
      { s: "boundary_crossing_object_large", a: { zone_id: "zone:gate", time_bucket: bucket }, lu: 100 },
    ],
  };
  const items = historyToTimelineItems(history, { maxEvents: 50 });
  assert.equal(items.length, 2);
  // newest first: the radar/adapter event
  assert.equal(items[0].modality.key, "radar");
  assert.equal(items[0].attestation.key, "adapter");
  // legacy event carries no indicators
  assert.equal(items[1].modality, null);
  assert.equal(items[1].attestation, null);
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

  // an unsigned publish (pre-PKI firmware, or a topic the firmware never signs)
  // is neutral: nothing was checked, so nothing failed. Distinct from both
  // "failed" (no ⚠) and "logged" (it names the reason).
  const unsigned = resolveVerification({ verified: false, trustReason: "unsigned" });
  assert.equal(unsigned.level, "unsigned");
  assert.equal(unsigned.label, "Unsigned");
  assert.notEqual(unsigned.symbol, "⚠");
  assert.notEqual(unsigned.symbol, "✓");
  // ...and the payload's own `signed` flag never outranks the verifier's verdict.
  assert.equal(resolveVerification({ verified: false, trustReason: "unsigned", signed: true }).level, "unsigned");
  // the verdicts that mean a check ran and rejected the publish stay failures.
  for (const reason of ["mismatch", "replay"]) {
    assert.equal(resolveVerification({ verified: false, trustReason: reason }).level, "failed", reason);
  }

  // kernel HTTP path with no signals → neutral "logged", never a green check.
  const logged = resolveVerification({});
  assert.equal(logged.level, "logged");
  assert.equal(logged.symbol, "·");
});

test("historyToTimelineItems renders an unsigned pre-PKI event as unsigned, not failed", () => {
  const bucket = { start_epoch_s: 600, size_s: 600 };
  const history = {
    "sensor.securacv_canary_old_last_event": [
      { s: "contact_state_change", a: { zone: "zone:door", verified: false, trust_reason: "unsigned", time_bucket: bucket }, lu: 100 },
      { s: "tamper_detected", a: { zone: "zone:door", verified: false, trust_reason: "mismatch", time_bucket: bucket }, lu: 200 },
    ],
  };
  const items = historyToTimelineItems(history, { maxEvents: 50 });
  assert.equal(items.length, 2);
  assert.equal(items[1].verification.level, "unsigned");
  assert.equal(items[0].verification.level, "failed");
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
