/*
 * securacv-strategy.js
 * ---------------------------------------------------------------------------
 * The SecuraCV dashboard strategy for Home Assistant: one generated dashboard
 * built from the entities the SecuraCV integration (and, before it claims the
 * kernel, the add-on's MQTT-discovery bridge) already exposes.
 *
 * Home Assistant loads this file as a module script (the integration registers
 * it through `add_extra_js_url`, beside the two cards) and finds the strategy
 * by its element name, `ll-strategy-dashboard-securacv`, when a dashboard says
 *
 *     strategy:
 *       type: custom:securacv
 *       frigate_url: /hassio/ingress/ccab4aaf_frigate   # optional
 *
 * Registering it in `window.customStrategies` also lists it under
 * Settings → Dashboards → Add dashboard → Community dashboards (Home Assistant
 * 2026.5 and later), so the user's one remaining step after the integration
 * connects is a click there.
 *
 * Design notes, kept deliberately in step with the rest of the repo:
 *   - No build step and no dependencies. Written without static import/export
 *     so the same file is a valid module script in the browser AND loads under
 *     Node (`require()` returns the pure helpers; the custom element registers
 *     only when a `customElements` registry exists) — the dual-use convention
 *     of the cards and viewer/verify_core.js. A syntax error here would blank
 *     every SecuraCV card too (the files share one `add_extra_js_url` path), so
 *     securacv-strategy.test.js import-evaluates the file.
 *   - Entities are selected by what the integration says they are — the entity
 *     registry's `platform == "securacv"` plus the translation key (contracts
 *     C11) — never by guessing at names. Without a registry the entity ids the
 *     integration generates are the fallback. Unclaimed bridge entities are the
 *     `pwk_*` ids the bridge's MQTT discovery asks for.
 *   - Honest copy. "Chain verified ✓" is the INTEGRATION's own walk against the
 *     key it pinned; the bridge's self-report reads "Chain OK (kernel-reported)".
 *     The strategy explains the coarsening once, says what a row is, and spells
 *     out the quiet-night / Frigate-silent / kernel-unreachable states instead
 *     of showing an empty list.
 *   - Privacy by construction: the dashboard shows the coarse claims the
 *     entities carry and Frigate's own camera pictures (Frigate's pixels, in
 *     Frigate's entity) — never a face, a plate, a name or a clip link that
 *     pretends the proof covers the pixels.
 */
(function () {
  "use strict";

  const STRATEGY_TYPE = "securacv";
  const ELEMENT_NAME = `ll-strategy-dashboard-${STRATEGY_TYPE}`;
  const PLATFORM = "securacv";
  const FRIGATE_PLATFORM = "frigate";
  const BRIDGE_PREFIX = "pwk_";
  const TIMELINE_CARD = "custom:securacv-timeline-card";
  // The Frigate app's ingress path on Home Assistant OS (the add-on slug is
  // fixed by the Frigate repository hash).
  const DEFAULT_FRIGATE_INGRESS = "/hassio/ingress/ccab4aaf_frigate";
  const DOCS_URL = "https://github.com/kmay89/securaCV/blob/main/docs/lovelace_timeline.md";

  // Translation keys (contracts C11) — roles on the kernel device.
  const KERNEL_ROLE_BY_KEY = {
    chain_valid: "chainValid",
    chain_length: "chainLength",
    daily_digest: "dailyDigest",
    kernel_last_event: "lastEvent",
    verify_now: "verifyNow",
    export_evidence: "exportEvidence",
    verification_events: "verificationEvents",
    kernel_online: "kernelOnline",
    frigate_link: "frigateLink",
    kernel_fingerprint: "fingerprint",
  };
  const KERNEL_WITNESS_KEY = "witness";
  const KERNEL_STORAGE_PREFIX = "storage_";
  // Keys that exist only on a kernel device (a Canary has `online`,
  // `last_event`, `chain_valid`, `chain_length`, `tamper*`).
  const KERNEL_ONLY_KEYS = new Set([
    KERNEL_WITNESS_KEY, "daily_digest", "kernel_last_event", "verify_now", "export_evidence",
    "verification_events", "kernel_online", "frigate_link", "kernel_fingerprint",
  ]);
  const CANARY_MARKER_KEYS = new Set(["online", "last_event", "tamper_events"]);

  // The two D13 label renames: the only words the dashboard spells itself. Every
  // other label is the entity's own `friendly_event`; these two are the honest
  // coarsening a Frigate user would otherwise read as a classification bug.
  const LABEL_OVERRIDES = {
    boundary_crossing_object_large: "Person or vehicle",
    boundary_crossing_object_small: "Animal or small object",
  };

  const UNAVAILABLE = new Set(["unavailable", "unknown", "none", "", null, undefined]);

  // --- small helpers ---------------------------------------------------------

  function objectId(entityId) {
    const i = String(entityId).indexOf(".");
    return i < 0 ? String(entityId) : String(entityId).slice(i + 1);
  }
  function domainOf(entityId) {
    const i = String(entityId).indexOf(".");
    return i < 0 ? "" : String(entityId).slice(0, i);
  }
  function stripZonePrefix(value) {
    const s = String(value);
    return s.startsWith("zone:") ? s.slice("zone:".length) : s;
  }
  /** The integration's zone slug rule (C11): lower-case, every non [a-z0-9] → `_`. */
  function slugify(value) {
    return String(value).toLowerCase().replace(/[^a-z0-9]/g, "_");
  }
  /** "front_door" → "Front door". */
  function humanize(slug) {
    const s = String(slug).replace(/[_-]+/g, " ").trim();
    return s ? s[0].toUpperCase() + s.slice(1) : s;
  }
  /**
   * Text that is going into a markdown card: no markup, and no template
   * opener survives (`{` and `%` are escaped, so `{{` can never render).
   */
  function mdText(value) {
    return String(value == null ? "" : value)
      .replace(/[<>&*_`[\]#{}%]/g, (c) => (c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : `\\${c}`));
  }
  /** An entity id or other short token inside a Jinja string literal. */
  function jinjaStr(value) {
    return `'${String(value == null ? "" : value).replace(/[\\'{}%]/g, "")}'`;
  }
  function stateOf(states, entityId) {
    const st = entityId && states[entityId];
    return st && typeof st === "object" ? st : null;
  }
  function attrsOf(states, entityId) {
    const st = stateOf(states, entityId);
    return (st && st.attributes) || {};
  }

  // --- model: what SecuraCV entities exist, and whose they are ---------------

  /**
   * Group the integration's entities by device, by translation key. Uses the
   * entity registry (`hass.entities`) when the frontend exposes it; otherwise
   * the ids the integration generates (device "SecuraCV" + has_entity_name →
   * `<domain>.securacv_<key>`, witness events `event.securacv_<zone>`).
   */
  function collectIntegrationEntities(hass) {
    const states = (hass && hass.states) || {};
    const registry = hass && hass.entities && typeof hass.entities === "object" ? hass.entities : null;
    const byDevice = new Map(); // device key → Map(translation_key → [entity ids])
    const add = (deviceKey, key, eid) => {
      if (!byDevice.has(deviceKey)) byDevice.set(deviceKey, new Map());
      const roles = byDevice.get(deviceKey);
      if (!roles.has(key)) roles.set(key, []);
      roles.get(key).push(eid);
    };
    if (registry) {
      for (const eid of Object.keys(registry)) {
        const row = registry[eid];
        if (!row || row.platform !== PLATFORM) continue;
        const key = row.translation_key || "";
        add(row.device_id || `entity:${eid}`, key, eid);
      }
      return { byDevice, hasRegistry: true };
    }
    // Fallback: the generated ids. Only the kernel device can be told apart
    // this way (Canary ids carry `securacv_canary_<id>_`).
    const KERNEL_SUFFIXES = {
      "binary_sensor.securacv_chain_valid": "chain_valid",
      "sensor.securacv_chain_length": "chain_length",
      "sensor.securacv_daily_digest": "daily_digest",
      "sensor.securacv_last_event": "kernel_last_event",
      "button.securacv_verify_now": "verify_now",
      "button.securacv_export_evidence": "export_evidence",
      "event.securacv_verification_events": "verification_events",
      "binary_sensor.securacv_kernel_online": "kernel_online",
      "binary_sensor.securacv_frigate_link": "frigate_link",
      "sensor.securacv_kernel_fingerprint": "kernel_fingerprint",
    };
    for (const eid of Object.keys(states)) {
      if (KERNEL_SUFFIXES[eid]) {
        add("kernel", KERNEL_SUFFIXES[eid], eid);
      } else if (domainOf(eid) === "event" && objectId(eid).startsWith("securacv_") && !objectId(eid).startsWith("securacv_canary_")) {
        add("kernel", KERNEL_WITNESS_KEY, eid);
      } else if (/^sensor\.securacv_storage_/.test(eid)) {
        add("kernel", `storage_${objectId(eid).slice("securacv_storage_".length)}`, eid);
      } else {
        const m = objectId(eid).match(/^securacv_canary_([a-z0-9]+)_(.+)$/);
        if (m) add(`canary:${m[1]}`, m[2], eid);
      }
    }
    return { byDevice, hasRegistry: false };
  }

  /** The per-camera witness entities of a kernel, with the Frigate camera beside each. */
  function witnessEntries(hass, eids) {
    const states = (hass && hass.states) || {};
    const registry = hass && hass.entities && typeof hass.entities === "object" ? hass.entities : null;
    const out = [];
    for (const eid of eids) {
      const attrs = attrsOf(states, eid);
      const zoneRaw = attrs.zone != null && attrs.zone !== "" ? attrs.zone : attrs.zone_id;
      const fromId = objectId(eid).replace(/^securacv_/, "");
      const zone = zoneRaw != null && zoneRaw !== "" ? stripZonePrefix(zoneRaw) : fromId;
      const slugs = [...new Set([slugify(zone), fromId])];
      let camera = null;
      for (const slug of slugs) {
        const camId = `camera.${slug}`;
        if (!stateOf(states, camId)) continue;
        const row = registry && registry[camId];
        // With a registry, Frigate's camera is the one Frigate owns; without
        // one, a camera by that name is the best available match.
        if (!row || row.platform === FRIGATE_PLATFORM) { camera = camId; break; }
      }
      const friendly = attrs.friendly_name ? String(attrs.friendly_name).replace(/^SecuraCV\s+/i, "") : "";
      out.push({ entityId: eid, zone, slug: slugs[0], name: friendly || humanize(zone), camera });
    }
    out.sort((a, b) => a.name.localeCompare(b.name));
    return out;
  }

  /** The Canary id behind a device's identifiers (securacv `canary_<id>`, firmware `securacv_<id>`). */
  function canaryIdOf(device) {
    const ids = device && Array.isArray(device.identifiers) ? device.identifiers : [];
    for (const pair of ids) {
      const value = Array.isArray(pair) ? String(pair[1]) : String(pair);
      const m = value.match(/^(?:securacv_canary_|canary_|securacv_)(.+)$/);
      if (m) return m[1];
    }
    return null;
  }

  /**
   * Everything the dashboard needs to know, in one pass:
   *   kernels[]  — the integration's kernel device(s) with its entities by role
   *   canaries[] — Canary groups: the integration's Canary device joined with
   *                the firmware's own MQTT-discovery device by the shared id
   *                (they are separate devices in the registry on purpose, D1)
   *   bridge     — the add-on's unclaimed `pwk_*` entities, when no kernel entry
   *                has claimed it yet
   */
  function collectModel(hass) {
    const states = (hass && hass.states) || {};
    const devices = (hass && hass.devices) || {};
    const registry = hass && hass.entities && typeof hass.entities === "object" ? hass.entities : null;
    const { byDevice, hasRegistry } = collectIntegrationEntities(hass);

    const kernels = [];
    const canaryByDevice = new Map();
    for (const [deviceKey, roles] of byDevice) {
      const keys = [...roles.keys()];
      const hasCanaryMarker = keys.some((k) => CANARY_MARKER_KEYS.has(k));
      // A kernel device is told by a kernel-only key; a kernel whose identity
      // has not answered yet (only chain_valid / chain_length / storage_* so
      // far) is still a kernel — a Canary always carries `online`.
      const isKernel = keys.some((k) => KERNEL_ONLY_KEYS.has(k))
        || (!hasCanaryMarker && keys.some((k) => k === "chain_valid" || k === "chain_length" || k.startsWith(KERNEL_STORAGE_PREFIX)));
      if (isKernel) {
        const kernel = { deviceId: hasRegistry ? deviceKey : null, entity: {}, storage: [], witness: [] };
        for (const [key, eids] of roles) {
          if (KERNEL_ROLE_BY_KEY[key]) kernel.entity[KERNEL_ROLE_BY_KEY[key]] = eids[0];
          else if (key === KERNEL_WITNESS_KEY) kernel.witness.push(...eids);
          else if (key.startsWith(KERNEL_STORAGE_PREFIX)) kernel.storage.push(...eids);
        }
        kernel.witness = witnessEntries(hass, kernel.witness.sort());
        kernel.storage.sort();
        kernels.push(kernel);
      } else if (hasCanaryMarker) {
        const entities = [...roles.values()].flat().sort();
        const device = hasRegistry ? devices[deviceKey] : null;
        const id = (device && canaryIdOf(device)) || (deviceKey.startsWith("canary:") ? deviceKey.slice(7) : null) || deviceKey;
        canaryByDevice.set(deviceKey, { id, deviceId: hasRegistry ? deviceKey : null, name: (device && (device.name_by_user || device.name)) || `Canary ${id}`, entities });
      }
    }

    // Join the firmware's MQTT-discovery devices by the shared id. Tier-1
    // devices are never linked to the kernel in the registry (2026.8's
    // single-config-entry rule), so the dashboard does the grouping.
    const groups = new Map();
    for (const canary of canaryByDevice.values()) {
      if (!groups.has(canary.id)) groups.set(canary.id, { id: canary.id, name: canary.name, deviceIds: [], entities: [], mqttEntities: [] });
      const g = groups.get(canary.id);
      if (canary.deviceId) g.deviceIds.push(canary.deviceId);
      g.entities.push(...canary.entities);
    }
    if (registry) {
      // Index the registry by device once; a home has thousands of entities.
      const visibleByDevice = new Map();
      for (const eid of Object.keys(registry)) {
        const row = registry[eid];
        if (!row || !row.device_id || row.hidden || row.entity_category || !stateOf(states, eid)) continue;
        if (!visibleByDevice.has(row.device_id)) visibleByDevice.set(row.device_id, []);
        visibleByDevice.get(row.device_id).push(eid);
      }
      for (const deviceId of Object.keys(devices)) {
        const device = devices[deviceId];
        const ids = device && Array.isArray(device.identifiers) ? device.identifiers : [];
        const isMqtt = ids.some((pair) => Array.isArray(pair) && pair[0] === "mqtt");
        if (!isMqtt) continue;
        const id = canaryIdOf(device);
        if (!id) continue;
        if (!groups.has(id)) groups.set(id, { id, name: device.name_by_user || device.name || `Canary ${id}`, deviceIds: [], entities: [], mqttEntities: [] });
        const g = groups.get(id);
        g.deviceIds.push(deviceId);
        g.mqttEntities.push(...(visibleByDevice.get(deviceId) || []));
        g.mqttEntities.sort();
      }
    }
    const canaries = [...groups.values()].sort((a, b) => a.name.localeCompare(b.name));

    // The unclaimed bridge: only while no kernel entry owns the kernel.
    let bridge = null;
    if (!kernels.length) {
      const b = { lastEvent: null, digest: null, chainProblem: null, verifyNow: null, zones: [] };
      const zones = new Map();
      for (const eid of Object.keys(states)) {
        const oid = objectId(eid);
        if (!oid.startsWith(BRIDGE_PREFIX)) continue;
        if (eid === "sensor.pwk_last_event") b.lastEvent = eid;
        else if (eid === "sensor.pwk_daily_digest") b.digest = eid;
        else if (eid === "binary_sensor.pwk_chain_problem") b.chainProblem = eid;
        else if (eid === "button.pwk_verify_now") b.verifyNow = eid;
        else {
          let m = oid.match(/^pwk_(.+)_events$/);
          if (m && domainOf(eid) === "sensor") {
            if (!zones.has(m[1])) zones.set(m[1], { zone: m[1], count: null, motion: null });
            zones.get(m[1]).count = eid;
          }
          m = oid.match(/^pwk_(.+)_motion$/);
          if (m && domainOf(eid) === "binary_sensor") {
            if (!zones.has(m[1])) zones.set(m[1], { zone: m[1], count: null, motion: null });
            zones.get(m[1]).motion = eid;
          }
        }
      }
      b.zones = [...zones.values()].sort((a, b2) => a.zone.localeCompare(b2.zone));
      if (b.lastEvent || b.digest || b.chainProblem || b.verifyNow || b.zones.length) bridge = b;
    }

    return { kernels, canaries, bridge, hasRegistry };
  }

  // --- options -------------------------------------------------------------

  /**
   * Where "Open Frigate" goes: the strategy's `frigate_url` option, else the
   * Frigate app's ingress path on Home Assistant OS (when the Supervisor is
   * present), else nowhere — no link is better than a dead one.
   */
  function resolveFrigateUrl(config, hass) {
    const configured = config && typeof config.frigate_url === "string" ? config.frigate_url.trim() : "";
    if (configured) return configured;
    const components = hass && hass.config && Array.isArray(hass.config.components) ? hass.config.components : [];
    return components.includes("hassio") ? DEFAULT_FRIGATE_INGRESS : null;
  }

  function linkAction(url) {
    return url.startsWith("/")
      ? { action: "navigate", navigation_path: url }
      : { action: "url", url_path: url };
  }

  // --- cards -----------------------------------------------------------------

  function heading(text, opts) {
    return Object.assign({ type: "heading", heading: text, heading_style: "title" }, opts || {});
  }
  function markdown(content) {
    return { type: "markdown", content };
  }
  function pressTile(entityId, name, icon) {
    return {
      type: "tile", entity: entityId, name, icon, vertical: false,
      tap_action: { action: "perform-action", perform_action: "button.press", target: { entity_id: entityId } },
      icon_tap_action: { action: "perform-action", perform_action: "button.press", target: { entity_id: entityId } },
    };
  }

  /** The chain-state header line, in the product's words (contracts C5, amended). */
  function chainHeaderMarkdown(kernel) {
    const cv = kernel.entity.chainValid;
    const cl = kernel.entity.chainLength;
    if (!cv && !cl) return "### Not verified yet";
    if (!cv) {
      return `{% set cl = ${jinjaStr(cl)} %}### Not verified yet{% if states(cl) | is_number %} · {{ '{:,}'.format(states(cl) | int(0)) }} entries{% endif %}`;
    }
    return [
      `{% set cv = ${jinjaStr(cv)} %}{% set cl = ${jinjaStr(cl || "")} %}`,
      "{% set st = states(cv) %}",
      "{% set vbs = state_attr(cv, 'verified_bucket_start') %}",
      "{% set n = state_attr(cv, 'verified_entries') %}",
      "{% if n is none and cl %}{% set n = states(cl) | int(0) %}{% endif %}",
      "{% set err = state_attr(cv, 'error') | string | lower %}",
      "{% set fs = state_attr(cv, 'failed_seq') %}",
      "{% if state_attr(cv, 'verifying') %}",
      "### Verifying…",
      "{% elif state_attr(cv, 'key_mismatch') is true or state_attr(cv, 'trust_reason') == 'kernel_key_mismatch' or ('key' in err and ('mismatch' in err or 'changed' in err)) %}",
      "### ⚠ Witness key changed — confirm it in Settings → Repairs",
      "{% elif st == 'on' and vbs is number %}",
      "### ✅ Chain verified ✓ {{ relative_time(as_datetime(vbs)) }} ago{% if n is number %} · {{ '{:,}'.format(n | int) }} entries{% endif %} · key pinned",
      "{% elif st == 'off' %}",
      "### ⚠ Verification failed{% if fs is number %} at entry #{{ '{:,}'.format(fs | int) }}{% endif %}",
      "{% else %}",
      "### Not verified yet",
      "{% endif %}",
    ].join("\n");
  }

  /** The bridge's self-report, never called verified. */
  function bridgeHeaderMarkdown(bridge) {
    const cp = bridge.chainProblem;
    const lines = [];
    if (cp) {
      lines.push(
        `{% set cp = ${jinjaStr(cp)} %}{% set n = state_attr(cp, 'events_verified') %}`,
        "{% if is_state(cp, 'on') %}### ⚠ Chain check failed (kernel-reported)",
        "{% elif is_state(cp, 'off') %}### Chain OK (kernel-reported){% if n is number %} · {{ '{:,}'.format(n | int) }} entries{% endif %}",
        "{% else %}### Not verified yet{% endif %}",
      );
    } else {
      lines.push("### Not verified yet");
    }
    lines.push(
      "",
      "This is the kernel's own report of its chain. Add the SecuraCV integration "
      + "(Settings → Devices & Services → Add integration → SecuraCV) for **Verified ✓** — a check Home Assistant "
      + "runs itself against the key it pinned — and one-tap evidence export.",
    );
    return lines.join("\n");
  }

  const COARSENING_COPY =
    "SecuraCV records coarse claims — \"person or vehicle\", a 10-minute window, a zone — never who or exactly "
    + "what. Frigate keeps the detail. One sealed event per camera, label and 10-minute window: three people at "
    + "the door in one window are one row.";

  /** Unreachable kernel / silent Frigate / the coarsening line, whichever applies. */
  function statusMarkdown(kernel) {
    const on = kernel.entity.kernelOnline;
    const fl = kernel.entity.frigateLink;
    const parts = [];
    if (on) parts.push(`{% set on = ${jinjaStr(on)} %}`);
    if (fl) parts.push(`{% set fl = ${jinjaStr(fl)} %}{% set heard = state_attr(fl, 'last_message_bucket_start') %}{% set pfx = state_attr(fl, 'topic_prefix') or 'frigate' %}`);
    const branches = [];
    if (on) {
      const devicePage = kernel.deviceId ? `/config/devices/device/${encodeURIComponent(kernel.deviceId)}` : "/config/integrations/integration/securacv";
      branches.push(
        "{% if is_state(on, 'off') %}"
        + "⚠ SecuraCV can't reach the witness kernel"
        + "{% set so = expand(on) %}{% if so %} since {{ as_local(so[0].last_changed).strftime('%H:%M') }}{% endif %}"
        + ` — [Open the SecuraCV device](${devicePage}).`,
      );
    }
    if (fl) {
      branches.push(
        `{% ${on ? "elif" : "if"} is_state(fl, 'off') %}`
        + "⚠ Frigate not heard{% if heard is number %} for {{ relative_time(as_datetime(heard)) }}{% endif %}. "
        + "Check Frigate → Settings → MQTT is enabled and publishes under `{{ pfx }}/` (SecuraCV listens on `{{ pfx }}/events`).",
      );
    }
    if (branches.length) {
      parts.push(branches.join("\n"), `{% else %}${COARSENING_COPY}{% endif %}`);
    } else {
      parts.push(COARSENING_COPY);
    }
    return parts.join("\n");
  }

  /** The quiet-night line shown while no camera has sealed an event yet. */
  function emptyTimelineMarkdown(kernel) {
    const fl = kernel.entity.frigateLink;
    if (!fl) {
      return "No witness events yet. The next person, car, dog or cat Frigate reports appears here sealed within a minute.";
    }
    return [
      `{% set fl = ${jinjaStr(fl)} %}{% set heard = state_attr(fl, 'last_message_bucket_start') %}`,
      "No witness events yet.",
      "{% if is_state(fl, 'on') and heard is number %} Frigate heard {{ relative_time(as_datetime(heard)) }} ago ✓ — the next person, car, dog or cat Frigate reports appears here sealed.",
      "{% elif is_state(fl, 'off') %} Frigate has not been heard — check Frigate → Settings → MQTT is enabled and publishes under `{{ state_attr(fl, 'topic_prefix') or 'frigate' }}/`.",
      "{% else %} The next person, car, dog or cat Frigate reports appears here sealed.{% endif %}",
    ].join("");
  }

  /** Per-camera detail: last event, its window, its badge, the 24 h count. */
  function cameraMarkdown(entry, digestEntity) {
    const labelMap = Object.entries(LABEL_OVERRIDES).map(([k, v]) => `${jinjaStr(k)}: ${jinjaStr(v)}`).join(", ");
    const lines = [
      `{% set e = ${jinjaStr(entry.entityId)} %}{% set tb = state_attr(e, 'time_bucket') %}{% set v = state_attr(e, 'verification') %}`,
      `{% set labels = {${labelMap}} %}`,
      "{% if states(e) in ['unknown', 'unavailable'] or tb is not mapping %}No sealed event yet.",
      "{% else %}**{{ labels.get(state_attr(e, 'event_type'), state_attr(e, 'friendly_event') or state_attr(e, 'event_type')) }}** · "
      + "{{ tb.start_epoch_s | timestamp_custom('%H:%M') }}–{{ (tb.start_epoch_s + (tb.size_s | default(600))) | timestamp_custom('%H:%M') }} · "
      + "{{ 'Verified ✓' if v == 'verified' else ('Failed ⚠' if v == 'failed' else 'Sealed ✓') }}"
      + "{% if state_attr(e, 'seq') is number %} · entry #{{ '{:,}'.format(state_attr(e, 'seq') | int) }}{% endif %}{% endif %}",
    ];
    if (digestEntity) {
      lines.push(
        `{% set pz = state_attr(${jinjaStr(digestEntity)}, 'per_zone') or {} %}`,
        `  \n{{ pz.get(${jinjaStr(`zone:${entry.zone}`)}, pz.get(${jinjaStr(entry.zone)}, 0)) }} in the last 24 h`,
      );
    }
    return lines.join("\n");
  }

  /** The rolling 24-hour digest, by period and by camera. */
  function digestMarkdown(digestEntity, chainValidEntity) {
    const lines = [
      `{% set d = ${jinjaStr(digestEntity)} %}`,
      "{% if states(d) in ['unknown', 'unavailable'] %}The digest has not been published yet — it refreshes with the kernel.",
      "{% else %}**{{ states(d) }}** sealed event{{ '' if states(d) == '1' else 's' }} in the last {{ (((state_attr(d, 'window_secs') or 86400) / 3600) | round(0)) | int }} hours",
      "{% set pp = state_attr(d, 'per_period') or {} %}{% if pp %}",
      "",
      "{% for p in ['night', 'morning', 'afternoon', 'evening'] if p in pp %}- {{ p }}: {{ pp[p] }}",
      "{% endfor %}{% endif %}",
      "{% set pz = state_attr(d, 'per_zone') or {} %}{% if pz %}",
      "",
      "{% for z, c in pz.items() %}- {{ z | replace('zone:', '') | replace('_', ' ') }}: {{ c }}",
      "{% endfor %}{% endif %}",
    ];
    if (chainValidEntity) {
      lines.push(
        `{% set cv = ${jinjaStr(chainValidEntity)} %}{% set vbs = state_attr(cv, 'verified_bucket_start') %}`,
        "{% if is_state(cv, 'on') and vbs is number %}",
        "",
        "Chain verified ✓ at {{ vbs | timestamp_custom('%H:%M') }}.",
        "{% endif %}",
      );
    }
    lines.push(
      "{% set lv = state_attr(d, 'last_verify') %}{% if lv is mapping and lv.verified_at_bucket_start is number %}",
      "",
      "Kernel's own last check (kernel-reported): {{ 'OK' if lv.chain_valid else 'failed' }} at {{ lv.verified_at_bucket_start | timestamp_custom('%H:%M') }}.",
      "{% endif %}",
      "",
      "Periods are the kernel's UTC quarters of the day; counts are coarse 10-minute windows, not detections.",
      "{% endif %}",
    );
    return lines.join("\n");
  }

  // --- sections --------------------------------------------------------------

  function headerSection(kernel, bridge) {
    const cards = [heading("SecuraCV", { icon: "mdi:shield-check" })];
    if (kernel) {
      cards.push(markdown(chainHeaderMarkdown(kernel)));
      cards.push(markdown(statusMarkdown(kernel)));
      const buttons = [];
      if (kernel.entity.verifyNow) buttons.push(pressTile(kernel.entity.verifyNow, "Verify now", "mdi:shield-search"));
      if (kernel.entity.exportEvidence) buttons.push(pressTile(kernel.entity.exportEvidence, "Export evidence", "mdi:file-certificate"));
      if (buttons.length) cards.push(...buttons);
      if (kernel.entity.exportEvidence) {
        cards.push(markdown(
          "Export evidence writes a signed pack of the last 24 hours — the bundle, the offline viewer and your witness "
          + "key fingerprint — and posts the download link as a notification (also under Media → SecuraCV evidence).",
        ));
      }
    } else if (bridge) {
      cards.push(markdown(bridgeHeaderMarkdown(bridge)));
      if (bridge.verifyNow) cards.push(pressTile(bridge.verifyNow, "Verify now", "mdi:shield-search"));
      cards.push(markdown(COARSENING_COPY));
    } else {
      cards.push(markdown(
        "No witness kernel is connected yet — only Canary devices. Chain state and sealed events per camera appear "
        + "here once the SecuraCV app (Home Assistant OS) or the Docker sidecar connects.",
      ));
    }
    return { type: "grid", column_span: 2, cards };
  }

  function todaySection(kernel, bridge, opts) {
    const cards = [heading("Today", { icon: "mdi:timeline-clock-outline" })];
    const timeline = { type: TIMELINE_CARD, title: "Verified timeline", hours: opts.hours, max_events: 50 };
    if (kernel) {
      const eventEntities = [...kernel.witness.map((w) => w.entityId)];
      if (kernel.entity.lastEvent) eventEntities.push(kernel.entity.lastEvent);
      if (!kernel.witness.length) cards.push(markdown(emptyTimelineMarkdown(kernel)));
      if (eventEntities.length) timeline.event_entities = eventEntities;
      if (kernel.entity.chainValid) timeline.chain_valid_entity = kernel.entity.chainValid;
      if (kernel.entity.chainLength) timeline.chain_length_entity = kernel.entity.chainLength;
    } else if (bridge) {
      if (bridge.lastEvent) timeline.event_entities = [bridge.lastEvent];
      if (bridge.chainProblem) timeline.chain_valid_entity = bridge.chainProblem;
    }
    // Without explicit entities the card discovers Canary rows on its own.
    cards.push(timeline);
    return { type: "grid", column_span: 2, cards };
  }

  function camerasSection(kernel, bridge, opts) {
    const cards = [heading("Cameras", { icon: "mdi:cctv" })];
    const frigateUrl = opts.frigateUrl;
    if (kernel) {
      for (const entry of kernel.witness) {
        const stack = [];
        if (entry.camera) {
          stack.push({ type: "picture-entity", entity: entry.camera, camera_view: "auto", show_name: false, show_state: false, tap_action: { action: "more-info" } });
        }
        stack.push({
          type: "tile", entity: entry.entityId, name: entry.name, icon: "mdi:cctv",
          state_content: ["last_changed"], tap_action: { action: "more-info" },
        });
        stack.push(markdown(cameraMarkdown(entry, kernel.entity.dailyDigest)));
        if (frigateUrl) {
          stack.push({ type: "button", name: "Open Frigate", icon: "mdi:open-in-new", show_state: false, icon_height: "24px", tap_action: linkAction(frigateUrl) });
        }
        cards.push({ type: "vertical-stack", cards: stack });
      }
    } else if (bridge) {
      for (const z of bridge.zones) {
        const stack = [];
        if (z.count) stack.push({ type: "tile", entity: z.count, name: `${humanize(z.zone)} · events`, icon: "mdi:cctv", tap_action: { action: "more-info" } });
        if (z.motion) stack.push({ type: "tile", entity: z.motion, name: "Recent activity", tap_action: { action: "more-info" } });
        if (frigateUrl) stack.push({ type: "button", name: "Open Frigate", icon: "mdi:open-in-new", show_state: false, icon_height: "24px", tap_action: linkAction(frigateUrl) });
        cards.push({ type: "vertical-stack", cards: stack });
      }
    }
    return cards.length > 1 ? { type: "grid", column_span: 2, cards } : null;
  }

  function digestSection(kernel, bridge) {
    const digest = kernel ? kernel.entity.dailyDigest : bridge && bridge.digest;
    if (!digest) return null;
    return {
      type: "grid",
      cards: [
        heading("Last 24 hours", { icon: "mdi:calendar-today" }),
        markdown(digestMarkdown(digest, kernel ? kernel.entity.chainValid : null)),
      ],
    };
  }

  function healthSection(kernel, bridge) {
    const rows = [];
    if (kernel) {
      for (const role of ["frigateLink", "kernelOnline", "chainLength", "fingerprint"]) {
        if (kernel.entity[role]) rows.push({ entity: kernel.entity[role] });
      }
      for (const eid of kernel.storage) rows.push({ entity: eid });
    } else if (bridge) {
      if (bridge.chainProblem) rows.push({ entity: bridge.chainProblem, name: "Chain problem (kernel-reported)" });
    }
    if (!rows.length) return null;
    return {
      type: "grid",
      cards: [heading("Health", { icon: "mdi:heart-pulse" }), { type: "entities", state_color: true, entities: rows }],
    };
  }

  function canariesSection(canaries) {
    if (!canaries.length) return null;
    const cards = [heading("Canaries", { icon: "mdi:bird" })];
    for (const group of canaries) {
      const rows = [];
      for (const eid of group.entities) rows.push({ entity: eid });
      const extra = group.mqttEntities.filter((eid) => !group.entities.includes(eid)).slice(0, 12);
      if (extra.length) {
        if (rows.length) rows.push({ type: "section", label: "Reported by the device" });
        for (const eid of extra) rows.push({ entity: eid });
      }
      if (!rows.length) continue;
      cards.push({ type: "entities", title: group.name, state_color: true, entities: rows });
    }
    return cards.length > 1 ? { type: "grid", column_span: 2, cards } : null;
  }

  function notSetUpSection() {
    return {
      type: "grid",
      column_span: 2,
      cards: [
        heading("SecuraCV", { icon: "mdi:shield-check" }),
        markdown(
          "**SecuraCV is not set up yet.** Add it under [Settings → Devices & Services](/config/integrations/dashboard/add?domain=securacv). "
          + "On Home Assistant OS, install the SecuraCV app from the App Store first — it announces its witness kernel and a "
          + "\"SecuraCV discovered\" card appears. Running Frigate under Docker? Start the SecuraCV sidecar beside it. "
          + "Canary devices appear here as they report.",
        ),
      ],
    };
  }

  // --- the strategy ----------------------------------------------------------

  /**
   * Build the dashboard. `config` is the strategy block (`frigate_url`,
   * `hours`); `hass` the Home Assistant object. Returns the Lovelace config:
   * one sections view, in the order header, Today, Cameras, Last 24 hours,
   * Health, Canaries — each section only when it has something true to say.
   */
  async function generate(config, hass) {
    const model = collectModel(hass);
    const hours = Number.isFinite(Number(config && config.hours)) && Number(config.hours) > 0 ? Number(config.hours) : 24;
    const opts = { frigateUrl: resolveFrigateUrl(config, hass), hours };
    const sections = [];
    if (!model.kernels.length && !model.bridge && !model.canaries.length) {
      sections.push(notSetUpSection());
    } else {
      const kernel = model.kernels[0] || null;
      const bridge = kernel ? null : model.bridge;
      sections.push(headerSection(kernel, bridge));
      if (model.kernels.length > 1) {
        sections[0].cards.push(markdown(
          `${model.kernels.length} witness kernels are connected; this dashboard shows the first. `
          + "Add a second dashboard with `strategy: {type: custom:securacv}` and pin cards by entity for the others.",
        ));
      }
      sections.push(todaySection(kernel, bridge, opts));
      for (const section of [camerasSection(kernel, bridge, opts), digestSection(kernel, bridge), healthSection(kernel, bridge), canariesSection(model.canaries)]) {
        if (section) sections.push(section);
      }
    }
    return {
      title: "SecuraCV",
      views: [{
        title: "Witness", path: "witness", icon: "mdi:shield-check",
        type: "sections", max_columns: 4, sections,
      }],
    };
  }

  // --- Node export surface (pure helpers only; no DOM) ----------------------
  const helpers = {
    STRATEGY_TYPE,
    ELEMENT_NAME,
    DEFAULT_FRIGATE_INGRESS,
    LABEL_OVERRIDES,
    collectModel,
    witnessEntries,
    canaryIdOf,
    resolveFrigateUrl,
    generate,
    slugify,
    humanize,
    mdText,
    jinjaStr,
  };
  if (typeof module !== "undefined" && module.exports) {
    module.exports = helpers;
  }

  // --- Browser registration --------------------------------------------------
  if (typeof HTMLElement === "undefined" || typeof customElements === "undefined") {
    return; // Node/test context: stop here, the helpers above are enough.
  }

  class SecuraCVDashboardStrategy extends HTMLElement {
    static async generate(config, hass) {
      return generate(config || {}, hass);
    }

    /** Defaults for the create-dashboard dialog; the user can change both. */
    static getCreateSuggestions() {
      return { title: "SecuraCV", icon: "mdi:shield-check" };
    }
  }
  // The only option (`frigate_url`) is YAML; there is no graphical editor.
  SecuraCVDashboardStrategy.noEditor = true;
  SecuraCVDashboardStrategy.configRequired = false;

  if (!customElements.get(ELEMENT_NAME)) {
    customElements.define(ELEMENT_NAME, SecuraCVDashboardStrategy);
  }
  window.customStrategies = window.customStrategies || [];
  if (!window.customStrategies.some((s) => s && s.type === STRATEGY_TYPE && s.strategyType === "dashboard")) {
    window.customStrategies.push({
      type: STRATEGY_TYPE,
      strategyType: "dashboard",
      name: "SecuraCV",
      description: "The verified witness log for Frigate: chain status, today's sealed events per camera, the daily digest, health, and your Canaries.",
      documentationURL: DOCS_URL,
    });
  }
})();
