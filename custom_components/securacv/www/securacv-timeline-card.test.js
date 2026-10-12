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
const fs = require("node:fs");
const path = require("node:path");

const {
  EVENT_TYPE_METADATA,
  normalizeEventType,
  eventMeta,
  normalizeModality,
  resolveModality,
  resolveAttestation,
  confidencePct,
  formatInt,
  formatTimeBucket,
  resolveTimeBucket,
  resolveZone,
  stripZonePrefix,
  resolveVerification,
  kernelVerification,
  normalizeHistoryEntry,
  historyToTimelineItems,
  discoverEntities,
  chainHeaderCopy,
  formatAgo,
  exportWindowHours,
  timelineStatus,
} = require("./securacv-timeline-card.js");

const DICTIONARY = path.join(__dirname, "..", "..", "..", "spec", "witness_dictionary.json");

test("EVENT_TYPE_METADATA keys are exactly the dictionary's event-type ids", () => {
  const dict = JSON.parse(fs.readFileSync(DICTIONARY, "utf8"));
  const ids = dict.event_types.items.map((e) => e.id).sort();
  assert.deepEqual(Object.keys(EVENT_TYPE_METADATA).sort(), ids);
});

test("normalizeEventType accepts snake_case and CamelCase enum forms", () => {
  assert.equal(normalizeEventType("acoustic_impulse_in_zone"), "acoustic_impulse_in_zone");
  assert.equal(normalizeEventType("AcousticImpulseInZone"), "acoustic_impulse_in_zone");
  assert.equal(normalizeEventType("BoundaryCrossingObjectLarge"), "boundary_crossing_object_large");
  assert.equal(normalizeEventType(""), "");
});

test("eventMeta resolves known types and falls back for unknown", () => {
  // The two boundary ids read as what a Frigate user saw — the honest
  // coarsening (person and car are one claim), not a classification bug.
  assert.deepEqual(eventMeta("boundary_crossing_object_large"), {
    label: "Person or vehicle",
    icon: "mdi:car",
  });
  assert.deepEqual(eventMeta("BoundaryCrossingObjectSmall"), {
    label: "Animal or small object",
    icon: "mdi:paw",
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

test("formatInt renders thousands separators and rejects junk", () => {
  assert.equal(formatInt(1204), "1,204");
  assert.equal(formatInt("42"), "42");
  assert.equal(formatInt(null), "");
  assert.equal(formatInt("nope"), "");
});

test("formatTimeBucket renders coarse windows, strings, and empties", () => {
  // 1970-01-01 00:10:00 UTC start; rendered in local time, so assert structure.
  const win = formatTimeBucket({ start_epoch_s: 600, size_s: 600 });
  assert.match(win, /^\d{2}:\d{2}–\d{2}:\d{2} \(~10 min\)$/);
  assert.equal(formatTimeBucket("yesterday morning"), "yesterday morning");
  assert.equal(formatTimeBucket(null), "");
  assert.equal(formatTimeBucket({ size_s: 600 }), "");
});

test("resolveTimeBucket accepts the nested object and the bridge's flattened pair", () => {
  const nested = { start_epoch_s: 600, size_s: 600 };
  assert.deepEqual(resolveTimeBucket({ time_bucket: nested }), nested);
  // The unclaimed bridge's MQTT attributes (`sensor.pwk_last_event`).
  assert.deepEqual(resolveTimeBucket({ time_bucket_start: 600, time_bucket_size: 600 }), nested);
  // Numeric strings (attributes round-tripped through a template) still count.
  assert.deepEqual(resolveTimeBucket({ time_bucket_start: "600", time_bucket_size: "600" }), nested);
  // The nested form wins when both are present (they carry the same numbers).
  assert.deepEqual(resolveTimeBucket({ time_bucket: nested, time_bucket_start: 1 }), nested);
  // A pre-formatted string passes through; nothing resolves to null.
  assert.equal(resolveTimeBucket({ time_bucket: "this morning" }), "this morning");
  assert.equal(resolveTimeBucket({}), null);
  assert.equal(resolveTimeBucket(null), null);
});

test("resolveZone prefers `zone`, strips the zone: prefix from zone_id, never shows the raw id", () => {
  assert.equal(stripZonePrefix("zone:front_door"), "front_door");
  assert.equal(stripZonePrefix("front_door"), "front_door");
  assert.equal(resolveZone({ zone: "front_door", zone_id: "zone:other" }), "front_door");
  assert.equal(resolveZone({ zone_id: "zone:front_door" }), "front_door");
  assert.equal(resolveZone({ zone: "", zone_id: "zone:gate" }), "gate");
  assert.equal(resolveZone({}), null);
  assert.equal(resolveZone(null), null);
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

test("resolveVerification: kernel rows get exactly three badges from `verification`", () => {
  // "verified" — the INTEGRATION's chain walk covered the entry.
  const verified = resolveVerification({ verification: "verified", verified: true, trustReason: "ha_verified_chain", seq: 1204, verifiedBucketStart: 600 });
  assert.equal(verified.level, "verified");
  assert.equal(verified.label, "Verified ✓");
  assert.equal(verified.symbol, "✓");
  assert.match(verified.tooltip, /^Entry #1,204 is covered by the chain check at \d{2}:\d{2} against the key pinned when you connected\.$/);

  // "sealed" — in the chain, newer than the last check; muted ✓, never green.
  const sealed = resolveVerification({ verification: "sealed", verified: false, trustReason: "kernel_sealed", seq: 1204 });
  assert.equal(sealed.level, "sealed");
  assert.equal(sealed.label, "Sealed ✓");
  // The cadence is the integration's (const.VERIFY_BACKSTOP_INTERVAL_SECONDS = 15 min,
  // VERIFY_IDLE_DELAY_SECONDS = 60 s), not the kernel's hourly self-check.
  assert.equal(sealed.tooltip, "Signed into the witness chain (entry #1,204). The chain check runs every 15 minutes and a minute after new entries stop arriving; this entry is newer than the last one.");

  // "failed" — the walk rejected the chain; the export nudge rides the tooltip.
  const failed = resolveVerification({ verification: "failed", verified: false, trustReason: "ha_verify_failed", seq: 1190 });
  assert.equal(failed.level, "failed");
  assert.equal(failed.label, "Failed ⚠");
  assert.equal(failed.symbol, "⚠");
  assert.equal(failed.tooltip, "The chain check rejected the chain at entry #1,190; later entries can't be trusted. Export now: entries before entry #1,190 still verify.");

  // The contracts-v1 spelling still maps to the same badge.
  assert.equal(resolveVerification({ verification: "verification_failed" }).level, "failed");

  // Without a seq the copy stays honest and omits the entry number.
  assert.equal(resolveVerification({ verification: "sealed" }).tooltip,
    "Signed into the witness chain. The chain check runs every 15 minutes and a minute after new entries stop arriving; this entry is newer than the last one.");
  assert.match(resolveVerification({ verification: "verified" }).tooltip, /^This entry is covered by the last chain check against the key pinned/);

  // A key change is never rendered as "Failed": the badge follows `verification`
  // and the tooltip points at Repairs.
  const keyChange = resolveVerification({ verification: "sealed", trustReason: "kernel_key_mismatch" });
  assert.equal(keyChange.level, "sealed");
  assert.match(keyChange.tooltip, /The witness key changed — confirm it in Settings → Repairs/);

  // `verification` outranks the Canary signals when both are present
  // (`verified: true` is set by the integration iff verification == verified).
  assert.equal(resolveVerification({ verification: "sealed", verified: true }).level, "sealed");
  // ...and a Canary row (no `verification`) is untouched by the kernel vocabulary.
  assert.equal(kernelVerification({ verified: true, trustReason: "pinned" }), null);
  assert.equal(kernelVerification({ verification: "made-up" }), null);
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
  // Canary rows carry no Export action — their publish is not in the kernel's log.
  assert.equal(items[0].kernel, false);
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
  assert.equal(items[0].zone, "b"); // the zone: prefix is never shown
  assert.equal(items[0].confidence, 70);
  assert.equal(items[0].verification.level, "signed");

  assert.equal(items[1].eventType, "boundary_crossing_object_large");
  assert.equal(items[1].verification.level, "verified");
  assert.equal(items[1].confidence, 90);
  assert.match(items[1].timeBucket, /~10 min/);
  assert.equal(items[1].bucketStart, 600);
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
  assert.deepEqual(items.map((i) => i.zone).sort(), ["driveway", "garden"]);
});

test("historyToTimelineItems: a kernel row seen through the sensor AND the event entity is one row", () => {
  const bucket = { start_epoch_s: 600, size_s: 600 };
  const kernelAttrs = {
    zone_id: "zone:front_door", zone: "front_door", time_bucket: bucket, confidence: 0.87,
    kernel_device_id: "pwk_1a2b3c4d", verification: "sealed", verified: false, trust_reason: "kernel_sealed", seq: 7,
    friendly_event: "Person or vehicle",
  };
  const history = {
    // The integration's last-event sensor: state is the event type.
    "sensor.securacv_last_event": [
      { s: "boundary_crossing_object_large", a: kernelAttrs, lu: 100 },
    ],
    // The per-camera event entity: state is a timestamp, the type is an attribute.
    "event.securacv_front_door": [
      { s: "2026-10-11T14:03:00+00:00", a: { ...kernelAttrs, event_type: "boundary_crossing_object_large" }, lu: 101 },
    ],
    // Two Canaries witnessing the same window in the same zone stay two rows:
    // their publishes carry no kernel_device_id, so the cross-entity de-dup
    // leaves them alone.
    "sensor.securacv_canary_a_last_event": [
      { s: "presence_in_restricted_zone", a: { zone_id: "zone:hall", time_bucket: bucket }, lu: 50 },
    ],
    "sensor.securacv_canary_b_last_event": [
      { s: "presence_in_restricted_zone", a: { zone_id: "zone:hall", time_bucket: bucket }, lu: 51 },
    ],
  };
  const items = historyToTimelineItems(history, { maxEvents: 50 });
  assert.equal(items.length, 3);
  const kernelRows = items.filter((i) => i.kernel);
  assert.equal(kernelRows.length, 1, "the sealed event is one row, not two");
  assert.equal(kernelRows[0].zone, "front_door");
  assert.equal(kernelRows[0].seq, 7);
  assert.equal(kernelRows[0].verification.label, "Sealed ✓");
  // The card's own words for a dictionary id, not the entity's friendly_event.
  assert.equal(kernelRows[0].label, "Person or vehicle");
  // An event entity with no event yet (state unknown) renders nothing.
  assert.deepEqual(historyToTimelineItems({ "event.securacv_x": [{ s: "unknown", a: {}, lu: 1 }] }, {}), []);
  // ...and one whose sample lacks event_type is skipped rather than labeled by its timestamp.
  assert.deepEqual(historyToTimelineItems({ "event.securacv_x": [{ s: "2026-10-11T14:03:00+00:00", a: {}, lu: 1 }] }, {}), []);
});

test("historyToTimelineItems: an unclaimed bridge row gets a window from the flattened bucket", () => {
  const history = {
    "sensor.pwk_last_event": [
      { s: "BoundaryCrossingObjectLarge", a: { zone_id: "zone:driveway", confidence: 0.9, time_bucket_start: 600, time_bucket_size: 600, device_id: "pwk_1a2b3c4d" }, lu: 100 },
    ],
  };
  const items = historyToTimelineItems(history, { maxEvents: 50 });
  assert.equal(items.length, 1);
  assert.equal(items[0].zone, "driveway");
  assert.match(items[0].timeBucket, /^\d{2}:\d{2}–\d{2}:\d{2} \(~10 min\)$/);
  assert.equal(items[0].label, "Person or vehicle");
  // No `verification` attribute: the bridge's self-report earns "Logged", never a ✓.
  assert.equal(items[0].verification.level, "logged");
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
    "event.doorbell": { state: "unknown", attributes: { event_types: ["pressed"] } },
  };
  const found = discoverEntities(states);
  assert.deepEqual(found.eventEntities, ["sensor.securacv_last_event"]);
  assert.equal(found.chainLengthEntity, "sensor.securacv_canary_abc_chain_length");
  assert.equal(found.chainValidEntity, "binary_sensor.securacv_canary_abc_chain_valid");
  assert.equal(found.chainValidInverted, false);
  assert.equal(found.tamperEntity, "binary_sensor.securacv_canary_abc_tamper");
});

test("discoverEntities: the entity registry's translation keys decide for the integration's entities", () => {
  const states = {
    // The integration names nothing "securacv … chain valid" in the attributes;
    // the registry says what each entity is.
    "binary_sensor.securacv_chain_valid": { state: "on", attributes: { friendly_name: "SecuraCV Chain valid" } },
    "sensor.securacv_chain_length": { state: "1204", attributes: { friendly_name: "SecuraCV Chain length" } },
    "sensor.securacv_last_event": { state: "boundary_crossing_object_large", attributes: { friendly_name: "SecuraCV Last event" } },
    "event.securacv_front_door": { state: "2026-10-11T14:03:00+00:00", attributes: { event_types: ["boundary_crossing_object_large", "boundary_crossing_object_small"] } },
    "event.securacv_driveway": { state: "unknown", attributes: { event_types: [] } },
    "event.securacv_verification_events": { state: "unknown", attributes: { event_types: ["chain_verified", "chain_verification_failed"] } },
    // An unclaimed bridge is also present (migration window): it must not
    // outrank the integration's verified entity.
    "binary_sensor.pwk_chain_problem": { state: "off", attributes: { device_class: "problem", chain_valid: true } },
  };
  const registry = {
    "binary_sensor.securacv_chain_valid": { platform: "securacv", translation_key: "chain_valid" },
    "sensor.securacv_chain_length": { platform: "securacv", translation_key: "chain_length" },
    "sensor.securacv_last_event": { platform: "securacv", translation_key: "kernel_last_event" },
    "event.securacv_front_door": { platform: "securacv", translation_key: "witness" },
    "event.securacv_driveway": { platform: "securacv", translation_key: "witness" },
    "event.securacv_verification_events": { platform: "securacv", translation_key: "verification_events" },
  };
  const found = discoverEntities(states, registry);
  assert.deepEqual(found.eventEntities.sort(), ["event.securacv_driveway", "event.securacv_front_door", "sensor.securacv_last_event"]);
  assert.equal(found.chainValidEntity, "binary_sensor.securacv_chain_valid");
  assert.equal(found.chainValidInverted, false);
  assert.equal(found.chainLengthEntity, "sensor.securacv_chain_length");
  // Without the registry the event entity is still found by its dictionary event_types.
  const bare = discoverEntities(states);
  assert.ok(bare.eventEntities.includes("event.securacv_front_door"));
  assert.ok(!bare.eventEntities.includes("event.securacv_verification_events"));
});

test("discoverEntities: an unclaimed bridge's pwk_chain_problem is the inverted chain entity", () => {
  const states = {
    "sensor.pwk_last_event": { state: "BoundaryCrossingObjectLarge", attributes: { zone_id: "zone:a", confidence: 0.9, time_bucket_start: 600, time_bucket_size: 600 } },
    "binary_sensor.pwk_chain_problem": { state: "off", attributes: { friendly_name: "PWK Chain Problem", device_class: "problem", chain_valid: true, events_verified: 1204 } },
    "button.pwk_verify_now": { state: "unknown", attributes: {} },
  };
  const found = discoverEntities(states);
  assert.deepEqual(found.eventEntities, ["sensor.pwk_last_event"]);
  assert.equal(found.chainValidEntity, "binary_sensor.pwk_chain_problem");
  assert.equal(found.chainValidInverted, true);
  assert.equal(found.tamperEntity, null);
});

test("chainHeaderCopy: the integration's walk speaks in the product's words", () => {
  const now = 600 + 12 * 60;
  const ok = chainHeaderCopy({
    chainValid: { state: "on", attributes: { verified_bucket_start: 600, verified_entries: 1204 }, inverted: false },
    chainLength: { state: "1204", attributes: { latest_hash: "abcd", pinned: true } },
    nowEpochS: now,
  });
  assert.equal(ok.cls, "ok");
  assert.equal(ok.text, "Chain verified ✓ 12 min ago · 1,204 entries · key pinned");

  // The entry count falls back to the chain-length sensor.
  const fromLength = chainHeaderCopy({
    chainValid: { state: "on", attributes: { verified_bucket_start: 600 }, inverted: false },
    chainLength: { state: "77", attributes: {} },
    nowEpochS: now,
  });
  assert.equal(fromLength.text, "Chain verified ✓ 12 min ago · 77 entries · key pinned");

  // A failed walk names the entry when it can.
  assert.equal(chainHeaderCopy({ chainValid: { state: "off", attributes: { failed_seq: 1190 } }, nowEpochS: now }).text,
    "Verification failed at entry #1,190");
  assert.equal(chainHeaderCopy({ chainValid: { state: "off", attributes: { error: "signature invalid at entry 1190" } }, nowEpochS: now }).text,
    "Verification failed at entry #1,190");
  const failed = chainHeaderCopy({ chainValid: { state: "off", attributes: {} }, nowEpochS: now });
  assert.equal(failed.text, "Verification failed");
  assert.equal(failed.cls, "warn");

  // Not yet, verifying, key changed.
  assert.equal(chainHeaderCopy({ chainValid: { state: "unknown", attributes: {} }, nowEpochS: now }).text, "Not verified yet");
  assert.equal(chainHeaderCopy({ chainLength: { state: "9", attributes: {} }, nowEpochS: now }).text, "Not verified yet · 9 entries");
  assert.equal(chainHeaderCopy({ chainValid: { state: "on", attributes: { verifying: true } }, nowEpochS: now }).text, "Verifying…");
  const key = chainHeaderCopy({ chainValid: { state: "off", attributes: { error: "verifying key mismatch" } }, nowEpochS: now });
  assert.equal(key.text, "Witness key changed — confirm it in Settings → Repairs");
  assert.equal(key.cls, "warn");
  assert.equal(chainHeaderCopy({ chainValid: { state: "on", attributes: { trust_reason: "kernel_key_mismatch" } }, nowEpochS: now }).text,
    "Witness key changed — confirm it in Settings → Repairs");

  // Nothing known → nothing said (the card shows its "no chain status" pill).
  assert.equal(chainHeaderCopy({}), null);
});

test("chainHeaderCopy: a kernel self-report is 'kernel-reported', never 'verified'", () => {
  const now = 600 + 30 * 60;
  const attrs = { device_class: "problem", chain_valid: true, events_verified: 1204, verified_at_bucket_start: 600 };
  const ok = chainHeaderCopy({ chainValid: { state: "off", attributes: attrs, inverted: true }, nowEpochS: now });
  assert.equal(ok.cls, "muted");
  assert.equal(ok.text, "Chain OK (kernel-reported) 30 min ago · 1,204 entries");
  assert.doesNotMatch(ok.text, /verified/i);

  const broken = chainHeaderCopy({ chainValid: { state: "on", attributes: { chain_valid: false }, inverted: true }, nowEpochS: now });
  assert.equal(broken.cls, "warn");
  assert.equal(broken.text, "Chain check failed (kernel-reported)");

  assert.equal(chainHeaderCopy({ chainValid: { state: "unknown", attributes: {}, inverted: true }, nowEpochS: now }).text, "Not verified yet");
});

test("formatAgo and exportWindowHours", () => {
  assert.equal(formatAgo(600, 600 + 12 * 60), "12 min ago");
  assert.equal(formatAgo(600, 600 + 3 * 3600), "3 h ago");
  assert.equal(formatAgo(600, 600 + 3 * 86400), "3 d ago");
  assert.equal(formatAgo(null, 600), "");

  // A whole hour past the event, clamped to the service's 1–720 h range.
  assert.equal(exportWindowHours(600, 600 + 30 * 60), 2);
  assert.equal(exportWindowHours(600, 600 + 5 * 3600), 6);
  assert.equal(exportWindowHours(600, 600 + 400 * 86400), 720);
  assert.equal(exportWindowHours(600, 600), 1);
  assert.equal(exportWindowHours(null, 600), 24);
});

test("timelineStatus: history read → no notice, honest window in the empty line", () => {
  const status = timelineStatus({ source: "history", hours: 24, count: 0 });
  assert.equal(status.notice, null);
  assert.match(status.empty, /last 24h/);
});

test("timelineStatus: nothing fetched yet → no notice", () => {
  const status = timelineStatus({ source: null, hours: 6, count: 0 });
  assert.equal(status.notice, null);
  assert.match(status.empty, /last 6h/);
});

test("timelineStatus: current-state fallback says so and claims no time window", () => {
  const status = timelineStatus({ source: "current-state", hours: 12, count: 0 });
  assert.ok(status.notice, "a fallback must be announced");
  assert.match(status.notice, /history/i);
  assert.match(status.notice, /current/);
  assert.match(status.notice, /12h/);
  assert.match(status.notice, /unavailable/);
  // The empty line must not pretend a window was read.
  assert.doesNotMatch(status.empty, /last \d+h/);
  assert.match(status.empty, /unavailable/i);
});

test("timelineStatus: current-state rows are still flagged when rows exist", () => {
  const status = timelineStatus({ source: "current-state", hours: 24, count: 3 });
  assert.ok(status.notice);
  assert.match(status.notice, /24h/);
});

test("timelineStatus: a missing or junk hours value falls back to the card default", () => {
  assert.match(timelineStatus({ source: "history" }).empty, /last 24h/);
  assert.match(timelineStatus({ source: "history", hours: "junk" }).empty, /last 24h/);
  assert.match(timelineStatus({ source: "current-state", hours: -1 }).notice, /24h/);
});

// --- The custom element itself, in a minimal fake DOM ------------------------
// The helpers above carry the logic; these drive the real element's lifecycle
// (setConfig / hass / the history fetch) for state that lives on the element.

const vm = require("node:vm");

// Evaluate the real card file with just enough DOM for it to register its
// element, and return the element class. A fresh sandbox per call, so no
// state leaks between tests and the Node-side helper import above is untouched.
function loadCardElement() {
  const src = fs.readFileSync(path.join(__dirname, "securacv-timeline-card.js"), "utf8");
  let registered = null;
  class FakeHTMLElement {
    constructor() { this.dispatched = []; }
    attachShadow() {
      this.shadowRoot = { innerHTML: "", querySelectorAll: () => [] };
      return this.shadowRoot;
    }
    dispatchEvent(ev) { this.dispatched.push(ev); return true; }
  }
  class FakeCustomEvent {
    constructor(type, init) { this.type = type; this.detail = (init || {}).detail; this.bubbles = Boolean((init || {}).bubbles); this.composed = Boolean((init || {}).composed); }
  }
  const sandbox = {
    HTMLElement: FakeHTMLElement,
    CustomEvent: FakeCustomEvent,
    customElements: { define: (_name, cls) => { registered = cls; } },
    console: { debug() {}, log() {}, warn() {}, error() {} },
    setTimeout: (fn) => ({ fn }), // never fires: notices stay put for the assertions
    clearTimeout() {},
    Date,
  };
  sandbox.window = sandbox;
  vm.runInNewContext(src, sandbox, { filename: "securacv-timeline-card.js" });
  assert.ok(registered, "the card file registered its custom element");
  return registered;
}

const settle = () => new Promise((resolve) => setImmediate(resolve));
const EVENT_ID = "sensor.securacv_last_event";
const eventStates = () => ({
  [EVENT_ID]: {
    state: "contact_state_change",
    attributes: { friendly_event: "Contact state change", zone_id: "zone:a", confidence: 0.8 },
    last_changed: "2026-09-23T10:00:00Z",
    last_updated: "2026-09-23T10:00:00Z",
  },
});
const NOTICE = /class="notice"/;

test("card: a failed history read's notice does not outlive the entities it was about", async () => {
  const Card = loadCardElement();
  const card = new Card();
  card.setConfig({}); // auto-discovery, so the entity set can change without setConfig

  card.hass = { states: eventStates(), callWS: () => Promise.reject(new Error("recorder off")) };
  await settle();
  assert.match(card.shadowRoot.innerHTML, NOTICE, "a failed read is announced");

  // The event sensor goes away: no history read is attempted for an empty
  // entity set, so nothing may still say history is unavailable.
  let calls = 0;
  const counting = () => { calls += 1; return Promise.resolve({}); };
  card.hass = { states: {}, callWS: counting };
  await settle();
  assert.equal(calls, 0);
  assert.doesNotMatch(card.shadowRoot.innerHTML, NOTICE);
  assert.doesNotMatch(card.shadowRoot.innerHTML, /unavailable/);

  // It comes back with the very same state: that is a fresh read, not a
  // reuse of the key the failed read left behind.
  card.hass = { states: eventStates(), callWS: counting };
  await settle();
  assert.equal(calls, 1, "the returning entity set re-reads history");
  assert.doesNotMatch(card.shadowRoot.innerHTML, NOTICE);
});

test("card: reconfiguring clears the previous config's history notice", async () => {
  const Card = loadCardElement();
  const card = new Card();
  card.setConfig({ event_entities: [EVENT_ID] });
  card.hass = { states: eventStates(), callWS: () => Promise.reject(new Error("recorder off")) };
  await settle();
  assert.match(card.shadowRoot.innerHTML, NOTICE);

  // New config, read still in flight (never settles here): the synchronous
  // render must not carry the old config's notice.
  card.setConfig({ event_entities: [EVENT_ID], hours: 6 });
  card.hass = { states: eventStates(), callWS: () => new Promise(() => {}) };
  assert.doesNotMatch(card.shadowRoot.innerHTML, NOTICE);
});

test("card: kernel rows render the three badges, a tap opens more-info, Export calls the service", async () => {
  const Card = loadCardElement();
  const card = new Card();
  card.setConfig({ event_entities: ["event.securacv_front_door"], chain_valid_entity: "binary_sensor.securacv_chain_valid", chain_length_entity: "sensor.securacv_chain_length" });
  const bucket = { start_epoch_s: Math.floor(Date.now() / 1000 / 600) * 600 - 600, size_s: 600 };
  const history = {
    "event.securacv_front_door": [
      { s: "2026-10-11T14:03:00+00:00", a: { event_type: "boundary_crossing_object_large", zone: "front_door", time_bucket: bucket, kernel_device_id: "pwk_1a2b3c4d", verification: "sealed", seq: 12 }, lu: 1 },
    ],
  };
  const calls = [];
  card.hass = {
    states: {
      "event.securacv_front_door": { state: "2026-10-11T14:03:00+00:00", attributes: history["event.securacv_front_door"][0].a, last_changed: "x", last_updated: "x" },
      "binary_sensor.securacv_chain_valid": { state: "on", attributes: { verified_bucket_start: bucket.start_epoch_s, verified_entries: 12 } },
      "sensor.securacv_chain_length": { state: "12", attributes: { latest_hash: "0123456789abcdef" } },
    },
    callWS: () => Promise.resolve(history),
    callService: (domain, service, data) => { calls.push([domain, service, data]); return Promise.resolve(); },
  };
  await settle();
  const html = card.shadowRoot.innerHTML;
  assert.match(html, /Chain verified ✓ \d+ min ago · 12 entries · key pinned/);
  assert.match(html, /head 01234567…/);
  assert.match(html, /Person or vehicle/);
  assert.match(html, /badge sealed/);
  assert.match(html, /Sealed ✓/);
  assert.match(html, /Signed into the witness chain \(entry #12\)/);
  assert.match(html, /data-entity="event.securacv_front_door"/);
  assert.match(html, /data-export="\d+"/);
  assert.doesNotMatch(html, /zone:front_door/);

  card._openMoreInfo("event.securacv_front_door");
  assert.equal(card.dispatched.length, 1);
  assert.equal(card.dispatched[0].type, "hass-more-info");
  // (the detail object was made in the sandbox realm — compare fields, not identity)
  assert.equal(card.dispatched[0].detail.entityId, "event.securacv_front_door");
  assert.ok(card.dispatched[0].bubbles && card.dispatched[0].composed);

  await card._exportFor(bucket.start_epoch_s);
  assert.equal(calls.length, 1);
  assert.deepEqual(calls[0].slice(0, 2), ["securacv", "export_evidence"]);
  assert.ok(calls[0][2].window_hours >= 1 && calls[0][2].window_hours <= 720);
  assert.match(card.shadowRoot.innerHTML, /Export requested/);
});

test("card: the export action reports a refusal instead of pretending", async () => {
  const Card = loadCardElement();
  const card = new Card();
  card.setConfig({});
  card.hass = { states: {}, callWS: () => Promise.resolve({}), callService: () => Promise.reject(new Error("no kernel")) };
  await card._exportFor(null);
  assert.match(card.shadowRoot.innerHTML, /Export failed/);
});
