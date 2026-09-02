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
 *   - Honest verification semantics. A green ✓ is shown ONLY when the event's
 *     own signature actually verified (the `verified` attribute the integration
 *     sets after an Ed25519 check). "signed but unverified", "unsigned" (there
 *     was no signature to check — pre-PKI firmware, or a topic the firmware
 *     never signs) and "logged via the kernel HTTP API" get weaker, distinct
 *     badges so the card never overclaims cryptographic proof it cannot back —
 *     and never calls a publish that was not checked a failure. Only a check
 *     that ran and rejected the publish (key mismatch, replay) is "failed".
 *     Consistent with the project's "documentation must not outrun the
 *     implementation" stance.
 *   - Privacy by construction. The card only ever displays coarse claims that
 *     the entities already carry (event_type, zone, coarse time bucket,
 *     confidence). It invents no precise time and surfaces no identity data.
 */
(function () {
  "use strict";

  // Mirror of custom_components/securacv/const.py EVENT_TYPE_METADATA. Kept in
  // sync by hand (small, stable vocabulary); the normalization below also
  // accepts the kernel's CamelCase enum form so either spelling resolves.
  const EVENT_TYPE_METADATA = {
    boundary_crossing_object_large: { label: "Large object crossed boundary", icon: "mdi:car" },
    boundary_crossing_object_small: { label: "Small object crossed boundary", icon: "mdi:paw" },
    acoustic_impulse_in_zone: { label: "Acoustic impulse in zone", icon: "mdi:waveform" },
    presence_in_restricted_zone: { label: "Presence in restricted zone", icon: "mdi:account-alert" },
    vehicle_presence_after_hours: { label: "Vehicle presence after hours", icon: "mdi:car-clock" },
    contact_state_change: { label: "Contact state change", icon: "mdi:door" },
    object_removed_from_zone: { label: "Object removed from zone", icon: "mdi:package-variant-closed-remove" },
    tamper_detected: { label: "Tamper detected", icon: "mdi:shield-alert" },
    vehicle_arrival_departure: { label: "Vehicle arrival/departure", icon: "mdi:car-side" },
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

  // States the recorder uses for "no real value" — never rendered as events.
  const NON_EVENT_STATES = new Set(["unavailable", "unknown", "none", "", null, undefined]);

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
    const meta = EVENT_TYPE_METADATA[key];
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
   * Resolve a verification badge from the signals a SecuraCV entity carries.
   * Returns { level, symbol, label, reason } where level is one of:
   *   "verified"   — signature checked and valid (strong green ✓)
   *   "signed"     — entry claims to be signed, no independent check available
   *   "unsigned"   — the publish carried no signature at all (pre-PKI firmware,
   *                  or a topic the firmware never signs); neutral, not a failure
   *   "logged"     — kernel HTTP path: present in the log, no per-event sig here
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
    if (s.verified === true) {
      return { level: "verified", symbol: "✓", label: "Signature verified", reason: s.trustReason || "verified" };
    }
    if (s.verified === false && s.trustReason === "unsigned") {
      return { level: "unsigned", symbol: "○", label: "Unsigned", reason: "unsigned" };
    }
    if (s.verified === false && s.trustReason && s.trustReason !== "no_pubkey") {
      return { level: "failed", symbol: "⚠", label: "Verification failed", reason: s.trustReason };
    }
    if (s.signed === true) {
      return { level: "signed", symbol: "✓", label: "Signed (unverified)", reason: s.trustReason || "no_pubkey" };
    }
    return { level: "logged", symbol: "·", label: "Logged", reason: s.trustReason || "kernel_api" };
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
   * Turn recorder history for the configured event entities into timeline
   * items, newest first. `history` is the map returned by
   * `history/history_during_period`: { entity_id: [entry, ...] }. In the compact
   * format attributes only appear when they change, so we carry them forward.
   *
   * Consecutive identical (state + bucket) samples are collapsed so a sensor
   * that re-publishes the same latest-event doesn't spam the timeline.
   */
  function historyToTimelineItems(history, opts) {
    const options = opts || {};
    const maxEvents = options.maxEvents || 50;
    const items = [];
    if (!history || typeof history !== "object") return items;

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
        const eventType = String(entry.state);
        const bucket = attrs.time_bucket;
        const zone = attrs.zone_id || attrs.zone || null;
        // Include zone in the de-dup key: coarse (~10 min) buckets mean two
        // genuinely distinct events of the same type in different zones can
        // share a bucket, and zone is user-visible metadata on the row — so
        // collapsing on type+bucket alone would hide real events.
        const dedupeKey = `${eventType}|${zone || ""}|${typeof bucket === "object" ? JSON.stringify(bucket) : bucket}`;
        if (dedupeKey === prevKey) continue;
        prevKey = dedupeKey;
        const meta = eventMeta(eventType);
        items.push({
          entityId,
          ts: entry.ts,
          eventType,
          label: attrs.friendly_event || meta.label,
          icon: meta.icon,
          zone,
          confidence: confidencePct(attrs.confidence),
          timeBucket: formatTimeBucket(bucket),
          modality: resolveModality(attrs),
          attestation: resolveAttestation(attrs),
          verification: resolveVerification({
            verified: attrs.verified,
            signed: attrs.signed,
            trustReason: attrs.trust_reason,
          }),
        });
      }
    }

    items.sort((a, b) => b.ts - a.ts);
    return items.slice(0, maxEvents);
  }

  /**
   * Best-effort discovery of the SecuraCV entities to bind when the card is
   * added with no explicit config. Matches on the integration's own attribute
   * signatures (which are specific to SecuraCV) plus an entity_id hint, so it
   * does not false-positive on unrelated entities.
   */
  function discoverEntities(states) {
    const out = { eventEntities: [], chainValidEntity: null, chainLengthEntity: null, tamperEntity: null };
    if (!states) return out;
    for (const entityId of Object.keys(states)) {
      const st = states[entityId];
      const attrs = (st && st.attributes) || {};
      const idHint = entityId.includes("securacv");
      if (entityId.startsWith("sensor.")) {
        const looksEvent =
          "friendly_event" in attrs ||
          (("zone" in attrs || "zone_id" in attrs) && "confidence" in attrs);
        if (looksEvent) out.eventEntities.push(entityId);
        else if ("latest_hash" in attrs && !out.chainLengthEntity) out.chainLengthEntity = entityId;
      } else if (entityId.startsWith("binary_sensor.")) {
        const name = String(attrs.friendly_name || entityId).toLowerCase();
        if ((idHint || name.includes("securacv")) && name.includes("chain") && name.includes("valid")) {
          out.chainValidEntity = out.chainValidEntity || entityId;
        } else if ((idHint || name.includes("securacv")) && name.includes("tamper")) {
          out.tamperEntity = out.tamperEntity || entityId;
        }
      }
    }
    return out;
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
    formatTimeBucket,
    resolveVerification,
    normalizeHistoryEntry,
    historyToTimelineItems,
    discoverEntities,
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
    .badge.signed { color: var(--primary-color, #03a9f4); }
    .badge.logged { color: var(--secondary-text-color); }
    .badge.unsigned { color: var(--secondary-text-color); font-weight: 500; }
    .badge.failed { color: var(--error-color, #e53935); }
    .conf { font-size: 0.75rem; color: var(--secondary-text-color); }
    .empty { padding: 24px 4px; text-align: center; color: var(--secondary-text-color); }
  `;

  class SecuraCVTimelineCard extends HTMLElement {
    constructor() {
      super();
      this.attachShadow({ mode: "open" });
      this._config = {};
      this._hass = null;
      this._items = [];
      this._historyKey = null;
      this._fetching = false;
      this._needReFetch = false;
    }

    setConfig(config) {
      this._config = Object.assign(
        { title: "Verified Timeline", hours: 24, max_events: 50 },
        config || {}
      );
      this._historyKey = null; // force a refresh on reconfigure
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
        return {
          eventEntities: cfg.event_entities
            ? Array.isArray(cfg.event_entities) ? cfg.event_entities : [cfg.event_entities]
            : [],
          chainValidEntity: cfg.chain_valid_entity || null,
          chainLengthEntity: cfg.chain_length_entity || null,
          tamperEntity: cfg.tamper_entity || null,
        };
      }
      return discoverEntities(this._hass ? this._hass.states : null);
    }

    async _maybeFetchHistory() {
      if (!this._hass) return;
      const entities = this._resolveEntities();
      const ids = entities.eventEntities;
      if (!ids.length) {
        this._items = [];
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
      } catch (err) {
        // Recorder may be disabled or the WS call unsupported; fall back to the
        // current state of each event entity so the card still shows something.
        this._items = this._itemsFromCurrentStates(ids);
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
      if (chainValid) {
        const ok = chainValid.state === "on";
        pills.push({ cls: ok ? "ok" : "warn", icon: ok ? "mdi:link-lock" : "mdi:link-off",
                     text: ok ? "Chain intact" : "Chain broken" });
      }

      const chainLen = entities.chainLengthEntity && hass.states[entities.chainLengthEntity];
      if (chainLen && !isUnavailable(chainLen.state)) {
        const head = chainLen.attributes && chainLen.attributes.latest_hash;
        const shortHead = head ? ` · ${String(head).slice(0, 8)}…` : "";
        pills.push({ cls: "muted", icon: "mdi:link-variant",
                     text: `${chainLen.state} blocks${shortHead}` });
      }

      const tamper = entities.tamperEntity && hass.states[entities.tamperEntity];
      if (tamper) {
        const alert = tamper.state === "on";
        pills.push({ cls: alert ? "warn" : "ok", icon: alert ? "mdi:alert" : "mdi:shield-check",
                     text: alert ? "Tamper detected" : "No tamper" });
      }
      return pills;
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
            return `
              <div class="event">
                <ha-icon icon="${it.icon}"></ha-icon>
                <div>
                  <div class="labelrow"><span class="label">${escapeHtml(it.label)}</span>${modality}</div>
                  <div class="meta">${escapeHtml(where)}${sep}${escapeHtml(when)}</div>
                </div>
                <div class="right">
                  <span class="badge ${it.verification.level}" title="${escapeHtml(it.verification.reason)}">${it.verification.symbol} ${escapeHtml(it.verification.label)}</span>
                  ${attest}
                  ${conf}
                </div>
              </div>`;
          }).join("")
        : `<div class="empty">No witness events in the last ${this._config.hours}h.</div>`;

      this.shadowRoot.innerHTML = `
        <style>${CARD_STYLE}</style>
        <ha-card>
          <div class="header"><span class="title">${escapeHtml(this._config.title)}</span></div>
          <div class="chain">${pillsHtml || '<span class="pill muted">No chain status entities found</span>'}</div>
          <div class="timeline">${eventsHtml}</div>
        </ha-card>`;
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
