/*
 * securacv-timeline-card.js
 * ---------------------------------------------------------------------------
 * A Lovelace card for the SecuraCV Home Assistant integration that renders a
 * verified-✓ event timeline plus a chain-status header from the entities the
 * integration already exposes (no new backend state required).
 *
 * Design notes, kept deliberately in step with the rest of the repo:
 *   - No build step and no dependencies. This file is served verbatim and is
 *     also importable under Node so the pure data-shaping helpers can be unit
 *     tested with `node --test` — the same dual-use convention as
 *     viewer/verify_core.js. The custom element only registers when a browser
 *     `customElements` registry exists.
 *   - Honest verification semantics, two vocabularies:
 *       * Kernel (Frigate) rows carry the integration's `verification`
 *         attribute and get exactly three badges — "Sealed ✓" (signed into
 *         the chain, not yet covered by a chain check), "Verified ✓" (covered
 *         by a chain check the INTEGRATION ran against the key it pinned) and
 *         "Failed ⚠" (that check rejected the chain). A kernel self-report is
 *         never rendered as verified; the header says "kernel-reported" when
 *         that is all there is.
 *       * Canary rows keep the per-publish signature vocabulary: a green ✓ is
 *         shown ONLY when the event's own Ed25519 signature verified (the
 *         `verified` attribute). "signed but unverified", "unsigned" and
 *         "logged" are weaker, distinct badges, and only a check that ran and
 *         rejected the publish (key mismatch, replay) is "failed".
 *     Consistent with the project's "documentation must not outrun the
 *     implementation" stance.
 *   - Privacy by construction. The card only ever displays coarse claims that
 *     the entities already carry (event_type, zone, coarse time bucket,
 *     confidence). It invents no precise time and surfaces no identity data.
 */
(function () {
  "use strict";

  // Mirror of custom_components/securacv/const.py EVENT_TYPE_METADATA keys
  // (scripts/lint_dictionary_sync.py pins the ids to the dictionary; the
  // labels are the card's own copy). The two boundary ids read as what a
  // Frigate user saw — a person or a car, an animal or a package — because
  // that is the honest coarsening, not a classification bug. The
  // normalization below also accepts the kernel's CamelCase enum form so
  // either spelling resolves.
  const EVENT_TYPE_METADATA = {
    boundary_crossing_object_large: { label: "Person or vehicle", icon: "mdi:car" },
    boundary_crossing_object_small: { label: "Animal or small object", icon: "mdi:paw" },
    acoustic_impulse_in_zone: { label: "Acoustic impulse in zone", icon: "mdi:waveform" },
    presence_in_restricted_zone: { label: "Presence in restricted zone", icon: "mdi:account-alert" },
    vehicle_presence_after_hours: { label: "Vehicle presence after hours", icon: "mdi:car-clock" },
    contact_state_change: { label: "Contact state change", icon: "mdi:door" },
    object_removed_from_zone: { label: "Object removed from zone", icon: "mdi:package-variant-closed-remove" },
    tamper_detected: { label: "Tamper detected", icon: "mdi:shield-alert" },
    vehicle_arrival_departure: { label: "Vehicle arrival/departure", icon: "mdi:car-side" },
  };
  // The system.integrity tamper KINDS (const.py's tamper vocabulary,
  // not dictionary event types — lint_dictionary_sync pins the table above
  // to the dictionary's ids, and these deliberately are not ids). They ride
  // the events wire with event_type = the kind word (csi_mqtt stamps
  // event_type from the state name), so eventMeta narrates each kind with
  // the phone app's exact calm sentences instead of a prettified raw id.
  const TAMPER_KIND_METADATA = {
    power_loss: { label: "Power was cut", icon: "mdi:shield-alert" },
    sd_remove: { label: "Storage card removed", icon: "mdi:shield-alert" },
    sd_error: { label: "Storage card failing", icon: "mdi:shield-alert" },
    watchdog: { label: "Recovered from a system hang", icon: "mdi:shield-alert" },
    unexpected_reboot: { label: "Rebooted unexpectedly", icon: "mdi:shield-alert" },
    enclosure: { label: "Enclosure opened", icon: "mdi:shield-alert" },
  };
  const DEFAULT_EVENT_ICON = "mdi:shield-eye";

  // Mirror of const.py MODALITY_METADATA + DEVICE_TYPE_MODALITY. A small
  // glyph distinguishes the sensing medium that produced a claim (radar reads
  // very differently from a camera even for the same coarse event_type). Kept
  // backward compatible: events with no resolvable modality get no indicator.
  const MODALITY_METADATA = {
    camera: { label: "Camera", icon: "mdi:camera" },
    "wifi-csi": { label: "WiFi CSI", icon: "mdi:wifi" },
    radar: { label: "Radar", icon: "mdi:radar" },
    contact: { label: "Contact", icon: "mdi:electric-switch" },
    other: { label: "Other sensor", icon: "mdi:access-point" },
  };
  const DEVICE_TYPE_MODALITY = {
    "canary-sense": "radar",
    "canary-vision": "camera",
    "canary-wap": "wifi-csi",
    "canary-contact": "contact",
    // Fuses several media, so no single-medium glyph (const.py explains).
    "canary-sentinel": "other",
  };
  const MODALITY_ALIASES = {
    csi: "wifi-csi",
    wifi: "wifi-csi",
    mmwave: "radar",
    "mmwave-radar": "radar",
    "60ghz": "radar",
    reed: "contact",
    door: "contact",
  };

  // Mirror of const.py ATTESTATION_METADATA. Orthogonal to the verify badge:
  // *who* signed the claim. Default "device" keeps device-signed events
  // unchanged; Track B (kit) claims render a distinct, honest provenance.
  const ATTESTATION_DEVICE = "device";
  const ATTESTATION_METADATA = {
    device: { label: "Device-attested", icon: "mdi:chip" },
    adapter: { label: "Adapter-attested", icon: "mdi:hub" },
    "ha-bridged": { label: "HA-bridged", icon: "mdi:home-assistant" },
  };

  // The integration's translation keys (contracts C11) — how the card tells a
  // kernel entity from a Canary one when Home Assistant hands it the entity
  // registry (`hass.entities`). Name heuristics stay as the fallback for
  // installs whose frontend does not expose the registry.
  const INTEGRATION_PLATFORM = "securacv";
  const KEYS = {
    witness: "witness",
    kernelLastEvent: "kernel_last_event",
    canaryLastEvent: "last_event",
    chainValid: "chain_valid",
    chainLength: "chain_length",
    tamper: "tamper",
  };
  // The unclaimed bridge's MQTT-discovery ids (contracts C3/C5): the kernel
  // publishes its own chain self-report there, so the card can show it — as
  // "kernel-reported", never as verified.
  const BRIDGE_ID_PREFIX = "pwk_";

  // States the recorder uses for "no real value" — never rendered as events.
  const NON_EVENT_STATES = new Set(["unavailable", "unknown", "none", "", null, undefined]);

  // The kernel's export window is bounded by the service (1–720 h, C7).
  const EXPORT_MIN_HOURS = 1;
  const EXPORT_MAX_HOURS = 720;

  /** Coerce a free-form modality to a known key, or null when unresolved. */
  function normalizeModality(value) {
    if (!value || typeof value !== "string") return null;
    const key = value.trim().toLowerCase().replace(/_/g, "-");
    if (Object.prototype.hasOwnProperty.call(MODALITY_METADATA, key)) return key;
    return MODALITY_ALIASES[key] || null;
  }

  /**
   * Resolve a sensing modality for an event from its attributes. Priority:
   * explicit `modality` attribute → the event's own `device_type`. Returns a
   * { key, label, icon } descriptor, or null when nothing resolves (the
   * backward-compatible "no indicator" case).
   */
  function resolveModality(attrs) {
    const a = attrs || {};
    const explicit = normalizeModality(a.modality);
    if (explicit) return { key: explicit, ...MODALITY_METADATA[explicit] };
    const dtype = a.device_type;
    if (typeof dtype === "string" && DEVICE_TYPE_MODALITY[dtype]) {
      const key = DEVICE_TYPE_MODALITY[dtype];
      return { key, ...MODALITY_METADATA[key] };
    }
    return null;
  }

  /**
   * Resolve attestation provenance from an event's attributes. Returns a
   * { key, label, icon } descriptor only when the event explicitly carries a
   * non-device attestation — device-attested (the default) renders no extra
   * provenance chip so existing events look exactly as before.
   */
  function resolveAttestation(attrs) {
    const a = attrs || {};
    const raw = a.attestation;
    if (!raw || typeof raw !== "string") return null;
    const key = raw.trim().toLowerCase().replace(/_/g, "-");
    if (key === ATTESTATION_DEVICE || !ATTESTATION_METADATA[key]) return null;
    return { key, ...ATTESTATION_METADATA[key] };
  }

  /** snake_case-normalize an event_type, accepting CamelCase enum names too. */
  function normalizeEventType(eventType) {
    if (!eventType) return "";
    const key = String(eventType).trim();
    if (Object.prototype.hasOwnProperty.call(EVENT_TYPE_METADATA, key)) return key;
    let snake = "";
    for (let i = 0; i < key.length; i++) {
      const ch = key[i];
      if (ch >= "A" && ch <= "Z" && i > 0) snake += "_";
      snake += ch.toLowerCase();
    }
    return snake;
  }

  /** {label, icon} for an event_type, falling back gracefully. */
  function eventMeta(eventType) {
    if (!eventType) return { label: "Unknown", icon: DEFAULT_EVENT_ICON };
    const key = normalizeEventType(eventType);
    const meta = EVENT_TYPE_METADATA[key] || TAMPER_KIND_METADATA[key];
    if (meta) return meta;
    return { label: String(eventType), icon: DEFAULT_EVENT_ICON };
  }

  /** Integer 0..100 percent for a [0,1] confidence, or null when unusable. */
  function confidencePct(confidence) {
    const n = typeof confidence === "string" ? parseFloat(confidence) : confidence;
    if (typeof n !== "number" || !isFinite(n)) return null;
    const clamped = Math.max(0, Math.min(1, n));
    return Math.round(clamped * 100);
  }

  /** A finite number from a number or numeric string, else null. */
  function toNumber(value) {
    if (typeof value === "number") return isFinite(value) ? value : null;
    if (typeof value === "string" && value.trim() !== "") {
      const n = Number(value);
      return isFinite(n) ? n : null;
    }
    return null;
  }

  /** "1,204" — thousands separators, locale-fixed so tests and copy agree. */
  function formatInt(value) {
    const n = toNumber(value);
    if (n == null) return "";
    return Math.round(n).toLocaleString("en-US");
  }

  /** Two-digit clock helper kept local so output is environment-stable. */
  function hhmm(epochSeconds) {
    const d = new Date(epochSeconds * 1000);
    const h = String(d.getHours()).padStart(2, "0");
    const m = String(d.getMinutes()).padStart(2, "0");
    return `${h}:${m}`;
  }

  /**
   * Human label for a coarse time bucket. Accepts the structured
   * {start_epoch_s, size_s} form (renders the window, e.g. "14:00–14:10"),
   * a pre-formatted string (returned as-is), or nothing.
   */
  function formatTimeBucket(bucket) {
    if (bucket == null) return "";
    if (typeof bucket === "string" || typeof bucket === "number") return String(bucket);
    const start = bucket.start_epoch_s;
    const size = bucket.size_s;
    if (typeof start !== "number") return "";
    if (typeof size === "number" && size > 0) {
      const mins = Math.round(size / 60);
      return `${hhmm(start)}–${hhmm(start + size)}${mins ? ` (~${mins} min)` : ""}`;
    }
    return hhmm(start);
  }

  /**
   * The coarse time bucket an event carries, in the structured form. The
   * integration always nests it (`time_bucket: {start_epoch_s, size_s}`,
   * contracts C5); the unclaimed bridge's MQTT attributes carry it flattened
   * (`time_bucket_start` / `time_bucket_size`). Both resolve here, so a
   * `sensor.pwk_last_event` row gets the same window a claimed one does. A
   * pre-formatted string is passed through untouched.
   */
  function resolveTimeBucket(attrs) {
    const a = attrs || {};
    const tb = a.time_bucket;
    if (tb && typeof tb === "object") return tb;
    if (typeof tb === "string" || typeof tb === "number") return tb;
    const start = toNumber(a.time_bucket_start);
    if (start != null) {
      const size = toNumber(a.time_bucket_size);
      return size != null ? { start_epoch_s: start, size_s: size } : { start_epoch_s: start };
    }
    return null;
  }

  /** Start epoch (seconds) of a resolved bucket, or null. */
  function bucketStart(bucket) {
    return bucket && typeof bucket === "object" ? toNumber(bucket.start_epoch_s) : null;
  }

  /** "zone:front_door" → "front_door"; anything else untouched. */
  function stripZonePrefix(value) {
    const s = String(value);
    return s.startsWith("zone:") ? s.slice("zone:".length) : s;
  }

  /**
   * The zone to show for an event: the integration's `zone` attribute (already
   * stripped, C5) first, else `zone_id` with its `zone:` prefix removed — so a
   * row reads "front_door", never "zone:front_door". Null when neither is set.
   */
  function resolveZone(attrs) {
    const a = attrs || {};
    const raw = a.zone != null && a.zone !== "" ? a.zone : a.zone_id;
    if (raw == null || raw === "") return null;
    return stripZonePrefix(raw);
  }

  /**
   * Resolve a verification badge from the signals a SecuraCV entity carries.
   * Returns { level, symbol, label, reason, tooltip } where level is one of:
   *   Kernel (Frigate) rows — from the integration's `verification` attribute:
   *   "verified"   — covered by a chain check the integration ran against the
   *                  key it pinned (green ✓, "Verified ✓")
   *   "sealed"     — signed into the witness chain, not yet covered by a
   *                  chain check (muted ✓, "Sealed ✓")
   *   "failed"     — the chain check rejected the chain (⚠, "Failed ⚠")
   *   Canary rows — from the per-publish signature check:
   *   "verified"   — signature checked and valid (strong green ✓)
   *   "signed"     — entry claims to be signed, no independent check available
   *   "unsigned"   — the publish carried no signature at all (pre-PKI firmware,
   *                  or a topic the firmware never signs); neutral, not a failure
   *   "logged"     — present in the log, no per-event signature surfaced here
   *   "failed"     — verification ran and rejected the publish: key mismatch,
   *                  bad signature, replay (⚠)
   * The card intentionally distinguishes these by label (the source of truth, since theme
   * colors can vary) so only the "Signature verified" badge means a real check; "signed"
   * reuses the ✓ glyph with a "Signed (unverified)" label and a distinct theme color, and
   * is never shown as the verified badge.
   *
   * "unsigned" is deliberately NOT "failed": nothing was checked, so nothing failed. The
   * integration's `trust_reason` vocabulary (device_trust.TrustVerdict) makes the split
   * exact — "unsigned" means the sig envelope was absent, "no_pubkey" means it was present
   * but no key is pinned yet, and every other false verdict ("mismatch", "replay") means a
   * check ran and said no. The payload's own `signed` flag never outranks the verifier.
   */
  function resolveVerification(signals) {
    const s = signals || {};
    const kernel = kernelVerification(s);
    if (kernel) return kernel;
    if (s.verified === true) {
      return { level: "verified", symbol: "✓", label: "Signature verified", reason: s.trustReason || "verified", tooltip: s.trustReason || "verified" };
    }
    if (s.verified === false && s.trustReason === "unsigned") {
      return { level: "unsigned", symbol: "○", label: "Unsigned", reason: "unsigned", tooltip: "unsigned" };
    }
    if (s.verified === false && s.trustReason && s.trustReason !== "no_pubkey") {
      return { level: "failed", symbol: "⚠", label: "Verification failed", reason: s.trustReason, tooltip: s.trustReason };
    }
    if (s.signed === true) {
      return { level: "signed", symbol: "✓", label: "Signed (unverified)", reason: s.trustReason || "no_pubkey", tooltip: s.trustReason || "no_pubkey" };
    }
    return { level: "logged", symbol: "·", label: "Logged", reason: s.trustReason || "kernel_api", tooltip: s.trustReason || "kernel_api" };
  }

  /** The three kernel levels; `verification_failed` is the v1 spelling of `failed`. */
  function normalizeKernelVerification(value) {
    if (typeof value !== "string") return null;
    const v = value.trim().toLowerCase();
    if (v === "sealed" || v === "verified" || v === "failed") return v;
    if (v === "verification_failed") return "failed";
    return null;
  }

  /**
   * The kernel-row badge, with the tooltip that explains the badge in plain
   * words. Returns null when the signals carry no `verification` attribute
   * (a Canary row — handled by the signature vocabulary above).
   *
   *   signals.verification         "sealed" | "verified" | "failed"
   *   signals.seq                  the entry's sealed-log row id, when known
   *   signals.verifiedBucketStart  bucket start of the integration's walk
   *   signals.trustReason          "kernel_key_mismatch" adds the Repairs hint
   */
  function kernelVerification(signals) {
    const s = signals || {};
    const level = normalizeKernelVerification(s.verification);
    if (!level) return null;
    const seq = toNumber(s.seq);
    const entryRef = seq != null ? `entry #${formatInt(seq)}` : null;
    const checkedAt = toNumber(s.verifiedBucketStart);
    const checkTime = checkedAt != null ? `the chain check at ${hhmm(checkedAt)}` : "the last chain check";
    const keyNote = s.trustReason === "kernel_key_mismatch"
      ? " The witness key changed — confirm it in Settings → Repairs before trusting new entries."
      : "";
    if (level === "verified") {
      const subject = entryRef ? `${entryRef[0].toUpperCase()}${entryRef.slice(1)} is` : "This entry is";
      return {
        level, symbol: "✓", label: "Verified ✓", reason: s.trustReason || "ha_verified_chain",
        tooltip: `${subject} covered by ${checkTime} against the key pinned when you connected.`,
      };
    }
    if (level === "failed") {
      const where = entryRef ? ` at ${entryRef}` : "";
      const before = entryRef ? `entries before ${entryRef} still verify` : "entries before the break still verify";
      return {
        level, symbol: "⚠", label: "Failed ⚠", reason: s.trustReason || "ha_verify_failed",
        tooltip: `The chain check rejected the chain${where}; later entries can't be trusted. Export now: ${before}.${keyNote}`,
      };
    }
    return {
      level, symbol: "✓", label: "Sealed ✓", reason: s.trustReason || "kernel_sealed",
      tooltip: `Signed into the witness chain${entryRef ? ` (${entryRef})` : ""}. The chain check runs every 15 minutes and a minute after new entries stop arriving; this entry is newer than the last one.${keyNote}`,
    };
  }

  function isUnavailable(state) {
    return NON_EVENT_STATES.has(typeof state === "string" ? state.toLowerCase() : state);
  }

  /**
   * Normalize one recorder history entry into { state, attributes, ts(ms) }.
   * Handles both the compact websocket shape ({ s, a, lu }) and the verbose
   * state-object shape ({ state, attributes, last_changed }).
   */
  function normalizeHistoryEntry(raw) {
    if (!raw || typeof raw !== "object") return null;
    if ("s" in raw || "lu" in raw) {
      const lu = typeof raw.lu === "number" ? raw.lu : 0;
      return { state: raw.s, attributes: raw.a || null, ts: Math.round(lu * 1000) };
    }
    const when = raw.last_changed || raw.last_updated;
    return {
      state: raw.state,
      attributes: raw.attributes || null,
      ts: when ? Date.parse(when) : 0,
    };
  }

  /**
   * The event type a history sample describes. A `sensor.*` last-event entity
   * carries it as the state; an `event.*` witness entity (contracts C10) has a
   * timestamp for a state and the type in its `event_type` attribute.
   */
  function sampleEventType(entityId, state, attrs) {
    if (String(entityId).startsWith("event.")) {
      const t = attrs && attrs.event_type;
      return t == null || t === "" ? null : String(t);
    }
    return String(state);
  }

  /**
   * Turn recorder history for the configured event entities into timeline
   * items, newest first. `history` is the map returned by
   * `history/history_during_period`: { entity_id: [entry, ...] }. In the compact
   * format attributes only appear when they change, so we carry them forward.
   *
   * Consecutive identical (state + zone + bucket) samples are collapsed so a
   * sensor that re-publishes the same latest-event doesn't spam the timeline,
   * and the same sealed event seen through two kernel entities (the last-event
   * sensor and the per-camera event entity both carry `kernel_device_id`) is
   * one row, not two.
   */
  function historyToTimelineItems(history, opts) {
    const options = opts || {};
    const maxEvents = options.maxEvents || 50;
    const items = [];
    if (!history || typeof history !== "object") return items;
    // Cross-entity de-dup is scoped by the device the claim came from, so two
    // Canaries witnessing the same zone in the same window stay two rows.
    const seenByDevice = new Set();

    for (const entityId of Object.keys(history)) {
      const series = history[entityId];
      if (!Array.isArray(series)) continue;
      let carriedAttrs = {};
      let prevKey = null;
      for (const raw of series) {
        const entry = normalizeHistoryEntry(raw);
        if (!entry) continue;
        if (entry.attributes) carriedAttrs = entry.attributes;
        if (isUnavailable(entry.state)) {
          prevKey = null;
          continue;
        }
        const attrs = carriedAttrs || {};
        const eventType = sampleEventType(entityId, entry.state, attrs);
        if (!eventType) continue;
        const bucket = resolveTimeBucket(attrs);
        const zone = resolveZone(attrs);
        // Include zone in the de-dup key: coarse (~10 min) buckets mean two
        // genuinely distinct events of the same type in different zones can
        // share a bucket, and zone is user-visible metadata on the row — so
        // collapsing on type+bucket alone would hide real events.
        const dedupeKey = `${normalizeEventType(eventType)}|${zone || ""}|${bucket && typeof bucket === "object" ? JSON.stringify(bucket) : bucket}`;
        if (dedupeKey === prevKey) continue;
        prevKey = dedupeKey;
        const kernelDevice = attrs.kernel_device_id;
        if (kernelDevice) {
          const deviceKey = `${kernelDevice}|${dedupeKey}`;
          if (seenByDevice.has(deviceKey)) continue;
          seenByDevice.add(deviceKey);
        }
        const meta = eventMeta(eventType);
        const known = Object.prototype.hasOwnProperty.call(EVENT_TYPE_METADATA, normalizeEventType(eventType));
        const verification = resolveVerification({
          verification: attrs.verification,
          verified: attrs.verified,
          signed: attrs.signed,
          trustReason: attrs.trust_reason,
          seq: attrs.seq,
          verifiedBucketStart: attrs.verified_bucket_start,
        });
        items.push({
          entityId,
          ts: entry.ts,
          eventType,
          // The card owns the words for the dictionary's ids; an entity's
          // friendly_event only names a type the card does not know.
          label: known ? meta.label : (attrs.friendly_event || meta.label),
          icon: meta.icon,
          zone,
          confidence: confidencePct(attrs.confidence),
          timeBucket: formatTimeBucket(bucket),
          bucketStart: bucketStart(bucket),
          seq: toNumber(attrs.seq),
          kernel: Boolean(kernelDevice) || normalizeKernelVerification(attrs.verification) != null,
          modality: resolveModality(attrs),
          attestation: resolveAttestation(attrs),
          verification,
        });
      }
    }

    items.sort((a, b) => b.ts - a.ts);
    return items.slice(0, maxEvents);
  }

  /** The registry row for an entity, when the frontend exposes the registry. */
  function registryEntry(registry, entityId) {
    if (!registry || typeof registry !== "object") return null;
    const row = registry[entityId];
    return row && typeof row === "object" ? row : null;
  }

  /** True when the registry says this entity belongs to the integration under `key`. */
  function isIntegrationEntity(registry, entityId, key) {
    const row = registryEntry(registry, entityId);
    return Boolean(row && row.platform === INTEGRATION_PLATFORM && (key == null || row.translation_key === key));
  }

  /** The object id of an entity id ("binary_sensor.pwk_chain_problem" → "pwk_chain_problem"). */
  function objectId(entityId) {
    const i = String(entityId).indexOf(".");
    return i < 0 ? String(entityId) : String(entityId).slice(i + 1);
  }

  /**
   * Best-effort discovery of the SecuraCV entities to bind when the card is
   * added with no explicit config. With the entity registry (`hass.entities`)
   * the integration's own translation keys decide (contracts C11); without it
   * the integration's attribute signatures (which are specific to SecuraCV)
   * plus an entity_id hint do, so it does not false-positive on unrelated
   * entities. An unclaimed bridge device is recognized by its `pwk_` ids: its
   * `binary_sensor.pwk_chain_problem` (device_class problem, `on` = broken)
   * is accepted as the chain entity with `chainValidInverted` set, and its
   * `/attrs` VerifyReport (the `chain_valid` attribute) is read for the copy.
   */
  function discoverEntities(states, registry) {
    const out = {
      eventEntities: [], chainValidEntity: null, chainValidInverted: false,
      chainLengthEntity: null, tamperEntity: null,
    };
    if (!states) return out;
    let pwkChain = null;
    for (const entityId of Object.keys(states)) {
      const st = states[entityId];
      const attrs = (st && st.attributes) || {};
      const idHint = entityId.includes("securacv");
      const fromBridge = objectId(entityId).startsWith(BRIDGE_ID_PREFIX);
      if (entityId.startsWith("sensor.")) {
        const looksEvent =
          isIntegrationEntity(registry, entityId, KEYS.kernelLastEvent) ||
          isIntegrationEntity(registry, entityId, KEYS.canaryLastEvent) ||
          "friendly_event" in attrs ||
          (("zone" in attrs || "zone_id" in attrs) && "confidence" in attrs);
        if (looksEvent) out.eventEntities.push(entityId);
        else if (isIntegrationEntity(registry, entityId, KEYS.chainLength)) {
          // The integration's own chain-length sensor outranks a name match.
          out.chainLengthEntity = entityId;
        } else if ("latest_hash" in attrs && !out.chainLengthEntity) out.chainLengthEntity = entityId;
      } else if (entityId.startsWith("event.")) {
        const types = attrs.event_types;
        const looksWitness =
          isIntegrationEntity(registry, entityId, KEYS.witness) ||
          (Array.isArray(types) && types.some((t) => Object.prototype.hasOwnProperty.call(EVENT_TYPE_METADATA, t)));
        if (looksWitness) out.eventEntities.push(entityId);
      } else if (entityId.startsWith("binary_sensor.")) {
        const name = String(attrs.friendly_name || entityId).toLowerCase();
        if (isIntegrationEntity(registry, entityId, KEYS.chainValid)) {
          if (!out.chainValidEntity || out.chainValidInverted) {
            out.chainValidEntity = entityId;
            out.chainValidInverted = false;
          }
        } else if ((idHint || name.includes("securacv")) && name.includes("chain") && name.includes("valid")) {
          if (!out.chainValidEntity || out.chainValidInverted) {
            out.chainValidEntity = entityId;
            out.chainValidInverted = false;
          }
        } else if (fromBridge && (objectId(entityId).includes("chain") || "chain_valid" in attrs)) {
          pwkChain = pwkChain || entityId;
        } else if (isIntegrationEntity(registry, entityId, KEYS.tamper) ||
                   ((idHint || name.includes("securacv")) && name.includes("tamper"))) {
          out.tamperEntity = out.tamperEntity || entityId;
        }
      }
    }
    // The bridge's self-report only when the integration has not claimed the
    // kernel — once it has, its chain_valid entity is the one that verified.
    if (!out.chainValidEntity && pwkChain) {
      out.chainValidEntity = pwkChain;
      out.chainValidInverted = true;
    }
    return out;
  }

  /** "12 min ago" / "3 h ago" / "2 d ago", or "" without a usable start. */
  function formatAgo(epochS, nowEpochS) {
    const start = toNumber(epochS);
    const now = toNumber(nowEpochS);
    if (start == null || now == null) return "";
    const mins = Math.max(0, Math.round((now - start) / 60));
    if (mins < 60) return `${mins} min ago`;
    const hours = Math.round(mins / 60);
    if (hours < 48) return `${hours} h ago`;
    return `${Math.round(hours / 24)} d ago`;
  }

  /** The entry number a failed walk names, from a `failed_seq` attribute or an error string. */
  function failedEntry(attrs) {
    const a = attrs || {};
    const direct = toNumber(a.failed_seq);
    if (direct != null) return direct;
    if (typeof a.error === "string") {
      const m = a.error.match(/(?:entry|seq(?:uence)?)\s*#?\s*(\d+)/i);
      if (m) return Number(m[1]);
    }
    return null;
  }

  /** True when the chain entity reports that the kernel's key no longer matches the pin. */
  function keyChanged(attrs) {
    const a = attrs || {};
    if (a.key_mismatch === true || a.trust_reason === "kernel_key_mismatch") return true;
    if (typeof a.error === "string") {
      const e = a.error.toLowerCase();
      return e.includes("key") && (e.includes("mismatch") || e.includes("changed") || e.includes("rotat"));
    }
    return false;
  }

  /**
   * The chain-status header line (contracts C5, amended): what the INTEGRATION's
   * own chain walk says, in the words the product uses everywhere —
   *   "Chain verified ✓ 12 min ago · 1,204 entries · key pinned"
   *   "Verifying…"
   *   "Verification failed at entry #1,190"
   *   "Not verified yet"
   *   "Witness key changed — confirm it in Settings → Repairs"
   * — and, for an unclaimed bridge device whose only signal is the kernel's own
   * self-report, "Chain OK (kernel-reported) · 1,204 entries": a self-report is
   * never rendered as verified (AGENTS.md rule 4).
   *
   *   chainValid   { state, attributes, inverted }  the chain entity, or null
   *   chainLength  { state, attributes }            the chain-length sensor, or null
   *   nowEpochS    seconds, for the "ago"
   * Returns { cls, icon, text } for a pill, or null when nothing is known.
   */
  function chainHeaderCopy({ chainValid, chainLength, nowEpochS } = {}) {
    const cv = chainValid || null;
    const cl = chainLength || null;
    if (!cv && !cl) return null;
    const attrs = (cv && cv.attributes) || {};
    const lenState = cl && !isUnavailable(cl.state) ? toNumber(cl.state) : null;
    const entries = toNumber(attrs.verified_entries) != null ? toNumber(attrs.verified_entries)
      : toNumber(attrs.events_verified) != null ? toNumber(attrs.events_verified)
      : lenState;
    const entriesPart = entries != null ? ` · ${formatInt(entries)} entries` : "";

    if (cv && cv.inverted) {
      // The bridge's `pwk_chain_problem`: `on` means broken; its attributes
      // are the kernel's own VerifyReport.
      const reportedOk = attrs.chain_valid === true || (attrs.chain_valid == null && cv.state === "off");
      if (cv.state === "on" || attrs.chain_valid === false) {
        return { cls: "warn", icon: "mdi:link-off", text: `Chain check failed (kernel-reported)${entriesPart}` };
      }
      if (isUnavailable(cv.state) && attrs.chain_valid == null) {
        return { cls: "muted", icon: "mdi:link-variant", text: "Not verified yet" };
      }
      const ago = formatAgo(attrs.verified_at_bucket_start != null ? attrs.verified_at_bucket_start : attrs.verified_bucket_start, nowEpochS);
      return {
        cls: "muted", icon: "mdi:link-variant",
        text: `Chain OK (kernel-reported)${reportedOk && ago ? ` ${ago}` : ""}${entriesPart}`,
      };
    }

    if (!cv) {
      return { cls: "muted", icon: "mdi:link-variant", text: `Not verified yet${entriesPart}` };
    }
    if (attrs.verifying === true) {
      return { cls: "muted", icon: "mdi:link-variant", text: "Verifying…" };
    }
    if (keyChanged(attrs)) {
      return { cls: "warn", icon: "mdi:key-alert", text: "Witness key changed — confirm it in Settings → Repairs" };
    }
    if (cv.state === "on") {
      const ago = formatAgo(attrs.verified_bucket_start, nowEpochS);
      return {
        cls: "ok", icon: "mdi:link-lock",
        text: `Chain verified ✓${ago ? ` ${ago}` : ""}${entriesPart} · key pinned`,
      };
    }
    if (cv.state === "off") {
      const at = failedEntry(attrs);
      return {
        cls: "warn", icon: "mdi:link-off",
        text: `Verification failed${at != null ? ` at entry #${formatInt(at)}` : ""}`,
      };
    }
    return { cls: "muted", icon: "mdi:link-variant", text: `Not verified yet${entriesPart}` };
  }

  /**
   * Hours to ask `securacv.export_evidence` for so the export covers an event
   * in the bucket starting at `bucketStartS`: a whole hour past the event,
   * clamped to the service's 1–720 h range. A row with no bucket exports the
   * default day.
   */
  function exportWindowHours(bucketStartS, nowEpochS) {
    const start = toNumber(bucketStartS);
    const now = toNumber(nowEpochS);
    if (start == null || now == null) return 24;
    const hours = Math.ceil(Math.max(0, now - start) / 3600) + 1;
    return Math.max(EXPORT_MIN_HOURS, Math.min(EXPORT_MAX_HOURS, hours));
  }

  // What the rows on screen are made of, and what to say about it.
  // `source` is what _maybeFetchHistory recorded:
  //   null            — nothing fetched yet;
  //   "history"       — history/history_during_period answered;
  //   "current-state" — that call failed (the recorder may be off, the WS
  //                     call unsupported, or a transient error) and the rows
  //                     are each event sensor's CURRENT state only.
  // Returns { notice, empty }: `notice` is rendered above the timeline
  // whenever it is non-null — also when rows exist, because those rows are
  // not a history and must be flagged as such — and `empty` is the line for
  // a timeline with no rows. The fallback wording says "unavailable", not
  // "the recorder is off": the catch also fires on transient errors, and it
  // never claims a time window it did not read.
  function timelineStatus({ source, hours } = {}) {
    const h = Number.isFinite(Number(hours)) && Number(hours) > 0 ? Number(hours) : 24;
    if (source === "current-state") {
      return {
        notice:
          "Event history is unavailable (the recorder may be off, or this Home " +
          "Assistant does not answer history queries), so this shows each " +
          `sensor's current event only — not what happened over the last ${h}h.`,
        empty: "History is unavailable and no sensor currently reports an event.",
      };
    }
    return { notice: null, empty: `No witness events in the last ${h}h.` };
  }

  // --- Node export surface (pure helpers only; no DOM) ----------------------
  const helpers = {
    EVENT_TYPE_METADATA,
    DEFAULT_EVENT_ICON,
    MODALITY_METADATA,
    DEVICE_TYPE_MODALITY,
    ATTESTATION_METADATA,
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
  };
  if (typeof module !== "undefined" && module.exports) {
    module.exports = helpers;
  }

  // --- Browser custom element ----------------------------------------------
  if (typeof HTMLElement === "undefined" || typeof customElements === "undefined") {
    return; // Node/test context: stop here, the helpers above are enough.
  }

  const CARD_STYLE = `
    :host { display: block; }
    ha-card { padding: 16px; }
    .header { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; }
    .title { font-size: 1.1rem; font-weight: 600; color: var(--primary-text-color); }
    .chain { margin: 12px 0 4px; display: flex; flex-wrap: wrap; gap: 8px; }
    .pill { display: inline-flex; align-items: center; gap: 6px; padding: 3px 10px;
            border-radius: 14px; font-size: 0.82rem; line-height: 1.4;
            background: var(--secondary-background-color); color: var(--primary-text-color); }
    .pill.ok { background: rgba(67,160,71,0.15); color: var(--success-color, #43a047); }
    .pill.warn { background: rgba(229,57,53,0.15); color: var(--error-color, #e53935); }
    .pill.muted { color: var(--secondary-text-color); }
    .timeline { margin-top: 12px; border-top: 1px solid var(--divider-color); }
    .event { display: grid; grid-template-columns: 28px 1fr auto; align-items: center;
             gap: 10px; padding: 10px 2px; border-bottom: 1px solid var(--divider-color); }
    .event[data-entity] { cursor: pointer; }
    .event[data-entity]:hover { background: var(--secondary-background-color); }
    .event ha-icon { color: var(--state-icon-color, var(--primary-text-color)); }
    .event .label { font-weight: 500; color: var(--primary-text-color); }
    .event .labelrow { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
    .event .meta { font-size: 0.8rem; color: var(--secondary-text-color); }
    .chip { display: inline-flex; align-items: center; gap: 3px; padding: 1px 7px;
            border-radius: 10px; font-size: 0.7rem; line-height: 1.5;
            background: var(--secondary-background-color); color: var(--secondary-text-color); }
    .chip ha-icon { --mdc-icon-size: 13px; width: 13px; height: 13px; }
    .chip.modality { color: var(--primary-text-color); }
    .chip.attest { color: var(--warning-color, #fb8c00);
                   background: rgba(251,140,0,0.14); }
    .event .right { text-align: right; display: flex; flex-direction: column; align-items: flex-end; gap: 2px; }
    .badge { font-size: 0.8rem; font-weight: 600; }
    .badge.verified { color: var(--success-color, #43a047); }
    .badge.sealed { color: var(--secondary-text-color); }
    .badge.signed { color: var(--primary-color, #03a9f4); }
    .badge.logged { color: var(--secondary-text-color); }
    .badge.unsigned { color: var(--secondary-text-color); font-weight: 500; }
    .badge.failed { color: var(--error-color, #e53935); }
    .conf { font-size: 0.75rem; color: var(--secondary-text-color); }
    .rowaction { font: inherit; font-size: 0.72rem; padding: 1px 8px; border-radius: 10px;
                 border: 1px solid var(--divider-color); background: transparent;
                 color: var(--primary-color, #03a9f4); cursor: pointer; }
    .rowaction:hover { background: var(--secondary-background-color); }
    .empty { padding: 24px 4px; text-align: center; color: var(--secondary-text-color); }
    .notice { padding: 8px 4px; font-size: 0.8rem; color: var(--secondary-text-color); }
  `;

  class SecuraCVTimelineCard extends HTMLElement {
    constructor() {
      super();
      this.attachShadow({ mode: "open" });
      this._config = {};
      this._hass = null;
      this._items = [];
      // "history" | "current-state" | null — see timelineStatus().
      this._historySource = null;
      this._historyKey = null;
      this._fetching = false;
      this._needReFetch = false;
      // One-line feedback after an Export row action; cleared by a timer.
      this._actionNotice = null;
      this._actionNoticeTimer = null;
    }

    setConfig(config) {
      this._config = Object.assign(
        { title: "Verified Timeline", hours: 24, max_events: 50 },
        config || {}
      );
      this._historyKey = null; // force a refresh on reconfigure
      // A new config has not read history yet: a notice left over from the
      // old one would describe a fetch this config never made.
      this._historySource = null;
    }

    getCardSize() {
      return 3 + Math.min(this._items.length, 6);
    }

    set hass(hass) {
      this._hass = hass;
      this._maybeFetchHistory();
      this._render();
    }

    _resolveEntities() {
      const cfg = this._config;
      if (cfg.event_entities || cfg.chain_valid_entity || cfg.chain_length_entity || cfg.tamper_entity) {
        const chainValidEntity = cfg.chain_valid_entity || null;
        return {
          eventEntities: cfg.event_entities
            ? Array.isArray(cfg.event_entities) ? cfg.event_entities : [cfg.event_entities]
            : [],
          chainValidEntity,
          // A hand-configured bridge entity is still the bridge's inverted
          // problem sensor; the id says so.
          chainValidInverted: Boolean(chainValidEntity && objectId(chainValidEntity).startsWith(BRIDGE_ID_PREFIX)),
          chainLengthEntity: cfg.chain_length_entity || null,
          tamperEntity: cfg.tamper_entity || null,
        };
      }
      return discoverEntities(this._hass ? this._hass.states : null, this._hass ? this._hass.entities : null);
    }

    async _maybeFetchHistory() {
      if (!this._hass) return;
      const entities = this._resolveEntities();
      const ids = entities.eventEntities;
      if (!ids.length) {
        // No event entities → no history read attempted, so no notice about
        // one (an earlier failed read must not keep speaking for this state).
        this._items = [];
        this._historySource = null;
        this._historyKey = null; // entities coming back must re-read, not reuse
        return;
      }
      // Re-fetch only when an event entity changes. The key folds in
      // last_updated (not just last_changed) because back-to-back events of
      // the same type are an attribute-only update — HA keeps last_changed but
      // advances last_updated — and we must not treat those as "no change".
      const key = ids.map((id) => {
        const st = this._hass.states[id];
        return st ? `${id}=${st.state}@${st.last_changed}#${st.last_updated}` : id;
      }).join("|");
      if (key === this._historyKey) return;
      // A change that arrives mid-fetch must not be dropped: remember that a
      // refresh is due and run it once the in-flight fetch settles.
      if (this._fetching) {
        this._needReFetch = true;
        return;
      }
      this._historyKey = key;
      this._fetching = true;
      this._needReFetch = false;
      try {
        const start = new Date(Date.now() - this._config.hours * 3600 * 1000).toISOString();
        const history = await this._hass.callWS({
          type: "history/history_during_period",
          start_time: start,
          entity_ids: ids,
          minimal_response: false,
          no_attributes: false,
          significant_changes_only: false,
        });
        this._items = historyToTimelineItems(history, { maxEvents: this._config.max_events });
        this._historySource = "history";
      } catch (err) {
        // Recorder may be disabled or the WS call unsupported; fall back to the
        // current state of each event entity so the card still shows something
        // — and SAY so: _render() puts a notice above these rows, because a
        // current-state snapshot presented as "the last N hours" is a false
        // statement about what happened.
        this._items = this._itemsFromCurrentStates(ids);
        this._historySource = "current-state";
        if (typeof console !== "undefined" && console.debug) {
          console.debug("securacv-timeline-card: history unavailable, showing current state only", err);
        }
      } finally {
        this._fetching = false;
        if (this._needReFetch) {
          this._maybeFetchHistory();
        } else {
          this._render();
        }
      }
    }

    _itemsFromCurrentStates(ids) {
      const synthetic = {};
      for (const id of ids) {
        const st = this._hass.states[id];
        if (!st || isUnavailable(st.state)) continue;
        synthetic[id] = [{ state: st.state, attributes: st.attributes, last_changed: st.last_changed }];
      }
      return historyToTimelineItems(synthetic, { maxEvents: this._config.max_events });
    }

    _chainStatusPills() {
      const hass = this._hass;
      const entities = this._resolveEntities();
      const pills = [];

      const chainValid = entities.chainValidEntity && hass.states[entities.chainValidEntity];
      const chainLen = entities.chainLengthEntity && hass.states[entities.chainLengthEntity];
      const header = chainHeaderCopy({
        chainValid: chainValid
          ? { state: chainValid.state, attributes: chainValid.attributes || {}, inverted: entities.chainValidInverted }
          : null,
        chainLength: chainLen ? { state: chainLen.state, attributes: chainLen.attributes || {} } : null,
        nowEpochS: Date.now() / 1000,
      });
      if (header) pills.push(header);

      if (chainLen && !isUnavailable(chainLen.state)) {
        const head = chainLen.attributes && chainLen.attributes.latest_hash;
        if (head) {
          pills.push({ cls: "muted", icon: "mdi:pound", text: `head ${String(head).slice(0, 8)}…` });
        }
      }

      const tamper = entities.tamperEntity && hass.states[entities.tamperEntity];
      if (tamper) {
        const alert = tamper.state === "on";
        pills.push({ cls: alert ? "warn" : "ok", icon: alert ? "mdi:alert" : "mdi:shield-check",
                     text: alert ? "Tamper detected" : "No tamper" });
      }
      return pills;
    }

    /** Tap on a row: Home Assistant's more-info dialog for the row's entity. */
    _openMoreInfo(entityId) {
      if (!entityId) return;
      this.dispatchEvent(new CustomEvent("hass-more-info", {
        bubbles: true, composed: true, detail: { entityId },
      }));
    }

    /**
     * Export row action: ask the kernel for a signed evidence pack whose window
     * covers the row's bucket. The service writes the zip, posts the signed
     * download link as a notification and lists it under Media (C7); the card
     * only says that it asked.
     */
    async _exportFor(bucketStartS) {
      if (!this._hass || typeof this._hass.callService !== "function") return;
      const windowHours = exportWindowHours(bucketStartS, Date.now() / 1000);
      try {
        await this._hass.callService("securacv", "export_evidence", { window_hours: windowHours });
        this._setActionNotice(`Export requested (last ${windowHours} h) — the download link arrives as a notification and under Media → SecuraCV evidence.`);
      } catch (err) {
        this._setActionNotice("Export failed — the SecuraCV integration refused the request; see Notifications and the log.");
        if (typeof console !== "undefined" && console.debug) {
          console.debug("securacv-timeline-card: export_evidence failed", err);
        }
      }
    }

    _setActionNotice(text) {
      this._actionNotice = text;
      if (this._actionNoticeTimer) clearTimeout(this._actionNoticeTimer);
      this._actionNoticeTimer = setTimeout(() => {
        this._actionNotice = null;
        this._actionNoticeTimer = null;
        this._render();
      }, 8000);
      this._render();
    }

    _wireRows() {
      const root = this.shadowRoot;
      if (!root || typeof root.querySelectorAll !== "function") return;
      root.querySelectorAll(".event[data-entity]").forEach((row) => {
        row.addEventListener("click", () => this._openMoreInfo(row.getAttribute("data-entity")));
      });
      root.querySelectorAll(".rowaction[data-export]").forEach((btn) => {
        btn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          const start = btn.getAttribute("data-export");
          this._exportFor(start === "" ? null : Number(start));
        });
      });
    }

    _render() {
      if (!this.shadowRoot) return;
      if (!this._hass) {
        this.shadowRoot.innerHTML = `<style>${CARD_STYLE}</style><ha-card></ha-card>`;
        return;
      }
      const pills = this._chainStatusPills();
      const pillsHtml = pills
        // Escape p.text: it carries device-supplied values (chain length,
        // latest_hash) that an untrusted local MQTT publisher controls, so it
        // must be treated as data, not markup — same as the event rows below.
        .map((p) => `<span class="pill ${p.cls}"><ha-icon icon="${p.icon}"></ha-icon>${escapeHtml(p.text)}</span>`)
        .join("");

      const status = timelineStatus({ source: this._historySource, hours: this._config.hours });
      const noticeHtml = status.notice ? `<div class="notice">${escapeHtml(status.notice)}</div>` : "";
      const actionHtml = this._actionNotice ? `<div class="notice">${escapeHtml(this._actionNotice)}</div>` : "";

      const eventsHtml = this._items.length
        ? this._items.map((it) => {
            const conf = it.confidence != null ? `<span class="conf">${it.confidence}% conf</span>` : "";
            const where = it.zone ? `${it.zone}` : "";
            const when = it.timeBucket || "";
            const sep = where && when ? " · " : "";
            // Modality chip beside the label: a glyph for the sensing medium
            // (radar / camera / WiFi-CSI / contact). Omitted when unresolved
            // so events with no modality info render exactly as before.
            const modality = it.modality
              ? `<span class="chip modality" title="${escapeHtml(it.modality.label)}"><ha-icon icon="${it.modality.icon}"></ha-icon>${escapeHtml(it.modality.label)}</span>`
              : "";
            // Attestation chip under the badge: honest provenance for Track B
            // (kit) claims signed at ingest, not on-device. Only shown when the
            // event is explicitly adapter/ha-bridged attested.
            const attest = it.attestation
              ? `<span class="chip attest" title="${escapeHtml(it.attestation.label)}"><ha-icon icon="${it.attestation.icon}"></ha-icon>${escapeHtml(it.attestation.label)}</span>`
              : "";
            // Export only where the kernel can answer: a Canary's own publish
            // is not in the kernel's sealed log.
            const exportBtn = it.kernel
              ? `<button class="rowaction" type="button" data-export="${it.bucketStart != null ? it.bucketStart : ""}" title="Export a signed evidence pack covering this window">Export</button>`
              : "";
            return `
              <div class="event" data-entity="${escapeHtml(it.entityId)}">
                <ha-icon icon="${it.icon}"></ha-icon>
                <div>
                  <div class="labelrow"><span class="label">${escapeHtml(it.label)}</span>${modality}</div>
                  <div class="meta">${escapeHtml(where)}${sep}${escapeHtml(when)}</div>
                </div>
                <div class="right">
                  <span class="badge ${it.verification.level}" title="${escapeHtml(it.verification.tooltip)}">${it.verification.symbol} ${escapeHtml(it.verification.label)}</span>
                  ${attest}
                  ${conf}
                  ${exportBtn}
                </div>
              </div>`;
          }).join("")
        : `<div class="empty">${escapeHtml(status.empty)}</div>`;

      this.shadowRoot.innerHTML = `
        <style>${CARD_STYLE}</style>
        <ha-card>
          <div class="header"><span class="title">${escapeHtml(this._config.title)}</span></div>
          <div class="chain">${pillsHtml || '<span class="pill muted">No chain status entities found</span>'}</div>
          ${noticeHtml}
          ${actionHtml}
          <div class="timeline">${eventsHtml}</div>
        </ha-card>`;
      this._wireRows();
    }
  }

  function escapeHtml(value) {
    return String(value == null ? "" : value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  customElements.define("securacv-timeline-card", SecuraCVTimelineCard);
  window.customCards = window.customCards || [];
  window.customCards.push({
    type: "securacv-timeline-card",
    name: "SecuraCV Verified Timeline",
    description: "Verified-✓ witness event timeline with hash-chain status.",
  });
})();
