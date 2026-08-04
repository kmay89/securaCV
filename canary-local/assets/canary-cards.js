// canary-local/assets/canary-cards.js — Canary Cards: the standardized
// widget-card layer (schema v1).
//
// One rule, borrowed from Home Assistant's discovery model: **one entity, one
// card**. A peripheral never ships bespoke UI — it publishes the same MQTT
// entity set it already announces to Home Assistant, and any surface (this
// teaching bench, the canary-display glass + its glass_web mirror, the
// companion app) renders those entities through this one card vocabulary.
// New peripheral = new entity list = cards for free. Nothing is rebuilt.
//
// The schema is deliberately tiny and JSON-serializable, so the display
// firmware's fleet model (fixed-size structs, no heap) can carry it and the
// wasm emulator can ingest it unchanged. The contract lives in
// docs/standard/CANARY_CARDS.md; tests/sense.test.js validates every card the
// Sense Lab builds against validateCard() below.
//
// Card kinds (v1):
//   binary     on/off state with severity color (occupancy, breathing lock…)
//   stat       a single number + unit (BPM, lux, heap…)
//   band       an ordered coarse vocabulary, current slot lit (near/mid/far)
//   sparkline  a stat with a short trend ring (P1 vitals, RSSI…)
//   event      last witness event + signature state
//   trust      chain length + verification badge (the trust surface)
//
// DOM-free: descriptor builders + validator are pure; only renderCard /
// renderCardGrid touch document, and they take it as an argument so Node
// tests can pass a stub.

export const CARD_SCHEMA_V = 1;

export const CARD_KINDS = ["binary", "stat", "band", "sparkline", "event", "trust"];

// Privacy classes, straight from the design doc's vocabulary (§2):
// P0 = coarse witness claims, P1 = opt-in wellbeing numerics, P2 = never
// leaves the device (rendered only as its coarse derivative).
export const PRIVACY_CLASSES = ["P0", "P1", "P2"];

// ----------------------------------------------------------------------------
// Validation — the schema contract, executable.
// ----------------------------------------------------------------------------

export function validateCard(c) {
  const errs = [];
  const need = (cond, msg) => { if (!cond) errs.push(msg); };

  need(c && typeof c === "object", "card must be an object");
  if (!c || typeof c !== "object") return errs;

  need(c.v === CARD_SCHEMA_V, `v must be ${CARD_SCHEMA_V}`);
  need(typeof c.id === "string" && /^[a-z0-9_]+$/.test(c.id),
    "id must be a lowercase slug (matches the HA object_id suffix)");
  need(CARD_KINDS.includes(c.kind), `kind must be one of ${CARD_KINDS.join("/")}`);
  need(typeof c.title === "string" && c.title.length > 0, "title required");
  if (c.privacy != null) need(PRIVACY_CLASSES.includes(c.privacy), "privacy must be P0/P1/P2");
  if (c.severity != null) need(["ok", "notice", "warn", "alert"].includes(c.severity), "bad severity");

  switch (c.kind) {
    case "binary":
      need(typeof c.state === "boolean" || c.state === null, "binary: state must be bool|null");
      break;
    case "stat":
    case "sparkline":
      need(typeof c.value === "number" || c.value === null, `${c.kind}: value must be number|null`);
      if (c.kind === "sparkline")
        need(Array.isArray(c.trend), "sparkline: trend array required");
      break;
    case "band":
      need(Array.isArray(c.options) && c.options.length >= 2, "band: options[] required");
      need(c.value === null || c.options.includes(c.value), "band: value must be an option or null");
      break;
    case "event":
      need(typeof c.value === "string" || c.value === null, "event: value must be string|null");
      break;
    case "trust":
      need(typeof c.chain === "number", "trust: chain length required");
      need(["verified", "signed", "unsigned", "failed", "unknown"].includes(c.badge),
        "trust: bad badge");
      break;
  }
  return errs;
}

// ----------------------------------------------------------------------------
// Icons — a tiny built-in vocabulary (inline SVG path data, 24×24 viewBox).
// Kept deliberately small; a surface may substitute its own glyphs by id.
// ----------------------------------------------------------------------------

export const ICONS = {
  radar: "M12 2a10 10 0 1 0 10 10h-2a8 8 0 1 1-8-8V2zm0 5a5 5 0 1 0 5 5h-2a3 3 0 1 1-3-3V7zm0 4a1 1 0 0 1 1 1h6l-2.5-2.5L18 8a8.5 8.5 0 0 0-6-2.5V11z",
  people: "M8 11a3 3 0 1 0-3-3 3 3 0 0 0 3 3zm8 0a3 3 0 1 0-3-3 3 3 0 0 0 3 3zM8 13c-2.7 0-6 1.3-6 4v2h12v-2c0-2.7-3.3-4-6-4zm8 0c-.5 0-1 0-1.5.1a4.7 4.7 0 0 1 2 3.9v2H22v-2c0-2.7-3.3-4-6-4z",
  ruler: "M3 17h18v4H3zM5 6l2 2 2-2 2 2 2-2 2 2 2-2 2 2V3H5z",
  lungs: "M9 3v7.2L6.8 8 4 10.8V21h5.5v-8H11V3zm6 0h-2v10h1.5v8H20V10.8L17.2 8 15 10.2z",
  heart: "M12 21S4 14.6 4 9.2A4.6 4.6 0 0 1 8.6 4.6 5 5 0 0 1 12 6a5 5 0 0 1 3.4-1.4A4.6 4.6 0 0 1 20 9.2C20 14.6 12 21 12 21z",
  sun: "M12 7a5 5 0 1 0 5 5 5 5 0 0 0-5-5zm0-5h1v3h-2V2zm0 17h1v3h-2v-3zM2 11h3v2H2zm17 0h3v2h-3zM4.9 4.9l1.4-1.4 2.1 2.1L7 7zm11.6 11.6 1.4-1.4 2.1 2.1-1.4 1.4zM4.9 19.1 7 17l1.4 1.4-2.1 2.1zm11.6-11.6L18.6 5l1.4 1.4-2.1 2.1z",
  link: "M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7 1.4 1.4L15 5a3 3 0 0 1 4.2 4.2l-3 3A3 3 0 0 1 11.4 12zM14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7-1.4-1.4L9 19a3 3 0 0 1-4.2-4.2l3-3A3 3 0 0 1 12.6 12z",
  alert: "M12 2 1 21h22zm0 6 1 7h-2zm0 9.5A1.3 1.3 0 1 1 10.7 19 1.3 1.3 0 0 1 12 17.5z",
  shield: "M12 2 4 5v6c0 5 3.4 9.7 8 11 4.6-1.3 8-6 8-11V5zm0 4.5 5 1.9V11c0 3.5-2.1 6.9-5 8.2-2.9-1.3-5-4.7-5-8.2V8.4z",
  clock: "M12 2a10 10 0 1 0 10 10A10 10 0 0 0 12 2zm1 5h-2v6l5 3 1-1.7-4-2.3z",
  chip: "M9 2h2v3h2V2h2v3h3a1 1 0 0 1 1 1v3h3v2h-3v2h3v2h-3v3a1 1 0 0 1-1 1h-3v3h-2v-3h-2v3H9v-3H6a1 1 0 0 1-1-1v-3H2v-2h3v-2H2V9h3V6a1 1 0 0 1 1-1h3zM8 8v8h8V8z",
  pulse: "M2 12h4l2-6 4 12 3-8 1.5 2H22v2h-6.5L14 12l-2.7 7.5L7.5 8.5 6.3 12H2z",
};

// ----------------------------------------------------------------------------
// Renderer — descriptors → DOM. Quiet Glass styling via .ccard-* classes
// (canary-local.css). doc defaults to globalThis.document for pages.
// ----------------------------------------------------------------------------

const SEV_CLASS = { ok: "cc-ok", notice: "cc-notice", warn: "cc-warn", alert: "cc-alert" };

function svgIcon(doc, id) {
  const svg = doc.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("class", "ccard-ic");
  svg.setAttribute("aria-hidden", "true");
  const path = doc.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", ICONS[id] || ICONS.chip);
  path.setAttribute("fill", "currentColor");
  svg.append(path);
  return svg;
}

function elc(doc, tag, cls, text) {
  const n = doc.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

export function renderCard(desc, doc = globalThis.document) {
  const errs = validateCard(desc);
  const card = elc(doc, "article", "ccard" + (desc.absent ? " ccard-absent" : ""));
  card.dataset.cardId = desc.id || "";
  card.dataset.cardKind = desc.kind || "";
  if (errs.length) {
    card.classList.add("ccard-invalid");
    card.append(elc(doc, "p", "muted", "invalid card: " + errs[0]));
    return card;
  }

  const head = elc(doc, "header", "ccard-head");
  head.append(svgIcon(doc, desc.icon));
  head.append(elc(doc, "h4", "ccard-title", desc.title));
  if (desc.privacy) head.append(elc(doc, "span", "ccard-priv ccard-priv-" + desc.privacy.toLowerCase(), desc.privacy));
  card.append(head);

  const body = elc(doc, "div", "ccard-body");
  const sev = desc.severity ? SEV_CLASS[desc.severity] : "";

  switch (desc.kind) {
    case "binary": {
      const on = desc.state === true;
      const pill = elc(doc, "span", "ccard-pill " + (desc.state === null ? "cc-null" : on ? (sev || "cc-ok") : "cc-idle"),
        desc.state === null ? "—" : on ? (desc.onLabel || "on") : (desc.offLabel || "off"));
      body.append(pill);
      break;
    }
    case "stat":
    case "sparkline": {
      const v = elc(doc, "div", "ccard-stat " + sev);
      v.append(elc(doc, "strong", null, desc.value === null ? "—" : String(desc.value)));
      if (desc.unit) v.append(elc(doc, "span", "ccard-unit", " " + desc.unit));
      body.append(v);
      if (desc.kind === "sparkline") body.append(renderSpark(desc.trend, doc));
      break;
    }
    case "band": {
      const row = elc(doc, "div", "ccard-band");
      for (const opt of desc.options) {
        row.append(elc(doc, "span", "ccard-slot" + (opt === desc.value ? " on " + (sev || "cc-ok") : ""), opt));
      }
      body.append(row);
      break;
    }
    case "event": {
      body.append(elc(doc, "code", "ccard-event", desc.value === null ? "—" : desc.value));
      if (desc.signed != null)
        body.append(elc(doc, "span", "ccard-signed " + (desc.signed ? "cc-ok" : "cc-warn"),
          desc.signed ? "✓ signed" : "unsigned"));
      break;
    }
    case "trust": {
      const badgeText = { verified: "✓ device-verified", signed: "signed", unsigned: "unsigned", failed: "FAILED", unknown: "…" };
      const badgeCls = { verified: "cc-ok", signed: "cc-notice", unsigned: "cc-warn", failed: "cc-alert", unknown: "cc-null" };
      body.append(elc(doc, "span", "ccard-pill " + badgeCls[desc.badge], badgeText[desc.badge]));
      body.append(elc(doc, "span", "ccard-chain", desc.chain + " links"));
      break;
    }
  }
  card.append(body);

  if (desc.absent) card.append(elc(doc, "p", "ccard-foot", "not in this build — discovery payload compiled out"));
  else if (desc.footer) card.append(elc(doc, "p", "ccard-foot", desc.footer));
  return card;
}

function renderSpark(trend, doc) {
  const W = 120, H = 28, PAD = 2;
  const svg = doc.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  svg.setAttribute("class", "ccard-spark");
  const vals = trend.filter((v) => typeof v === "number");
  if (vals.length >= 2) {
    const min = Math.min(...vals), max = Math.max(...vals);
    const span = max - min || 1;
    const pts = vals.map((v, i) =>
      `${PAD + (i * (W - 2 * PAD)) / (vals.length - 1)},${H - PAD - ((v - min) / span) * (H - 2 * PAD)}`);
    const line = doc.createElementNS("http://www.w3.org/2000/svg", "polyline");
    line.setAttribute("points", pts.join(" "));
    line.setAttribute("fill", "none");
    line.setAttribute("stroke", "currentColor");
    line.setAttribute("stroke-width", "1.5");
    svg.append(line);
  }
  return svg;
}

export function renderCardGrid(descs, doc = globalThis.document) {
  const grid = elc(doc, "div", "ccard-grid");
  for (const d of descs) grid.append(renderCard(d, doc));
  return grid;
}

// ----------------------------------------------------------------------------
// The canary-sense card set — one card per HA entity, driven by the same
// generated entity list the firmware announces (devices/sense.json).
// ----------------------------------------------------------------------------
//
// snapshot: the live SenseSnapshot-shaped object (sense-sim.js pipeline).
// meta: { vitalsBuild, p1OptIn, chain: {length, badge}, trend: {breath, heart, lux} }

export function senseCards(snapshot, meta) {
  const s = snapshot;
  const cards = [];

  cards.push({
    v: 1, id: "presence", kind: "binary", icon: "radar", title: "Presence",
    state: s.presence === "unknown" ? null : s.presence === "present",
    onLabel: "present", offLabel: "clear",
    severity: s.presence === "present" ? "ok" : null,
    privacy: "P0", footer: "debounced radar presence — binary_sensor, occupancy",
  });

  cards.push({
    v: 1, id: "occupants", kind: "band", icon: "people", title: "Occupants",
    options: ["0", "1", "2+"], value: s.presence === "unknown" ? null : s.count,
    privacy: "P0", footer: "bucketed count — never a track log",
  });

  cards.push({
    v: 1, id: "range_band", kind: "band", icon: "ruler", title: "Range band",
    options: ["near", "mid", "far"], value: s.range === "unknown" ? null : s.range,
    privacy: "P2", footer: "raw centimeters never leave the device — band only",
  });

  cards.push({
    v: 1, id: "radar_link", kind: "binary", icon: "link", title: "Radar link",
    state: s.radar_ok, onLabel: "ok", offLabel: "stalled",
    severity: s.radar_ok ? "ok" : "alert",
    privacy: "P0", footer: "problem sensor — ON while the UART is silent",
  });

  cards.push({
    v: 1, id: "frame_errors", kind: "stat", icon: "pulse", title: "Frame errors",
    value: s.frame_errors, unit: "", severity: s.frame_errors > 0 ? "notice" : null,
    privacy: "P0", footer: "checksum drops, monotonic — radar-link health",
  });

  cards.push({
    v: 1, id: "illuminance", kind: "stat", icon: "sun", title: "Illuminance",
    value: typeof s.lux === "number" && s.lux >= 0 ? Math.round(s.lux) : null, unit: "lx",
    privacy: "P0", footer: "BH1750 — lights-out + presence = tamper corroboration",
  });

  const vitalsBuild = !!meta.vitalsBuild;
  cards.push({
    v: 1, id: "breathing", kind: "binary", icon: "lungs", title: "Breathing confirmed",
    state: vitalsBuild ? !!s.breathing_locked : null,
    onLabel: "locked", offLabel: "—",
    severity: vitalsBuild && s.breathing_locked ? "ok" : null,
    privacy: "P0", absent: !vitalsBuild,
    footer: "the P0 wellbeing binary — a lock, never a number",
  });

  const p1 = vitalsBuild && !!meta.p1OptIn;
  cards.push({
    v: 1, id: "breath_rate", kind: "sparkline", icon: "lungs", title: "Breath rate",
    value: p1 && s.bpm_valid ? s.breath_bpm : null, unit: "bpm",
    trend: p1 ? meta.trend.breath : [],
    privacy: "P1", absent: !p1,
    footer: "wellbeing signal, non-diagnostic — never sealed-logged",
  });

  cards.push({
    v: 1, id: "heart_rate", kind: "sparkline", icon: "heart", title: "Heart rate",
    value: p1 && s.bpm_valid ? s.heart_bpm : null, unit: "bpm",
    trend: p1 ? meta.trend.heart : [],
    privacy: "P1", absent: !p1,
    footer: "wellbeing signal, non-diagnostic — never sealed-logged",
  });

  cards.push({
    v: 1, id: "last_event", kind: "event", icon: "clock", title: "Last event",
    value: s.last_event || null, signed: meta.chain ? meta.chain.signed : null,
    privacy: "P0", footer: "presence transitions only — 10-minute time buckets",
  });

  cards.push({
    v: 1, id: "chain", kind: "trust", icon: "shield", title: "Witness chain",
    chain: meta.chain ? meta.chain.length : 0,
    badge: meta.chain ? meta.chain.badge : "unknown",
    privacy: "P0", footer: "Ed25519 hash chain — HA TOFU-pins the pubkey",
  });

  return cards;
}
