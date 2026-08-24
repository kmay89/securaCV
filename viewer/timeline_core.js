'use strict';

// Timeline scrub-view model — the pure data-shaping half of the viewer's
// "day shape" timeline (the Sublime-Text-minimap-inspired scrub view in
// template.html). No DOM here: everything is plain data in, plain data out,
// so it runs under `node --test` (timeline_core.test.js) and in the browser
// alike — the same dual-use convention as verify_core.js.
//
// Vocabulary note: EVENT_TYPE_META below is a display mirror of
// spec/witness_dictionary.json event_types (keys are the canonical snake_case
// ids; labels are the canonical presentation labels). It is held in sync by
// scripts/lint_dictionary_sync.py — edit the dictionary first, then this map.
// The `family` field is presentation-only grouping for the timeline's color
// system (three categorical hues + the page's reserved status roles for
// tamper and gaps), not vocabulary.

// ---- vocabulary presentation -------------------------------------------

const EVENT_TYPE_META = {
  boundary_crossing_object_large: { label: 'Large object crossed boundary', family: 'move' },
  boundary_crossing_object_small: { label: 'Small object crossed boundary', family: 'move' },
  acoustic_impulse_in_zone: { label: 'Acoustic impulse in zone', family: 'sound' },
  presence_in_restricted_zone: { label: 'Presence in restricted zone', family: 'touch' },
  vehicle_presence_after_hours: { label: 'Vehicle presence after hours', family: 'touch' },
  contact_state_change: { label: 'Contact state change', family: 'touch' },
  object_removed_from_zone: { label: 'Object removed from zone', family: 'touch' },
  tamper_detected: { label: 'Tamper detected', family: 'tamper' },
  vehicle_arrival_departure: { label: 'Vehicle arrival/departure', family: 'move' },
};

// Human names for the color families (the timeline legend). Tamper and gaps
// ride the page's existing status colors and always carry a glyph + label —
// never color alone.
const FAMILY_LABELS = {
  move: 'Crossings & movement',
  touch: 'Presence & touch',
  sound: 'Sound',
  tamper: 'Tamper',
  gap: 'Gaps',
  other: 'System records',
};

// Kernel wire strings are the Rust enum's PascalCase variant names; the
// dictionary ids are snake_case. Accept either spelling (same normalization
// stance as the Home Assistant timeline card).
function normalizeTypeId(t) {
  if (typeof t !== 'string' || !t) return '';
  return t.trim().replace(/([a-z0-9])([A-Z])/g, '$1_$2').toLowerCase();
}

function typeMeta(t) {
  const id = normalizeTypeId(t);
  const known = Object.prototype.hasOwnProperty.call(EVENT_TYPE_META, id) ? EVENT_TYPE_META[id] : null;
  if (known) return { id, label: known.label, family: known.family };
  // Unknown/future types stay honest: a readable generic label, neutral color.
  return { id, label: humanizeWords(t), family: 'other' };
}

// "StorageFull" / "storage_full" -> "Storage full". Generic — failure types
// carry no display metadata in the dictionary, so nothing here can drift.
function humanizeWords(t) {
  if (typeof t !== 'string' || !t) return 'Unknown';
  const words = normalizeTypeId(t).split('_').filter(Boolean);
  if (!words.length) return 'Unknown';
  const s = words.join(' ');
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// ---- record normalization ----------------------------------------------

// Both bundle shapes reduce to one record shape:
//   { t0, size, kind, type, label, family, zone, conf, details }
// kind: 'event' | 'gap' (a sealed failure record — a declared blind spot)
//     | 'system' (key rotation / lifecycle) | 'heartbeat'.
// t0/size come from the privacy-coarsened time bucket (typically 600 s) —
// the model never invents precision the log does not carry.

function bucketOf(rec) {
  const tb = rec && rec.time_bucket;
  if (!tb || typeof tb.start_epoch_s !== 'number') return null;
  return {
    t0: tb.start_epoch_s,
    size: typeof tb.size_s === 'number' && tb.size_s > 0 ? tb.size_s : 600,
  };
}

function recordFromEvent(e) {
  const b = bucketOf(e);
  if (!b) return null;
  const meta = typeMeta(typeof e.event_type === 'string' ? e.event_type : String(e.event_type));
  return {
    t0: b.t0, size: b.size, kind: 'event',
    type: e.event_type, label: meta.label, family: meta.family,
    zone: typeof e.zone_id === 'string' ? e.zone_id : '',
    conf: typeof e.confidence === 'number' ? e.confidence : null,
    details: '',
  };
}

function recordFromFailure(f) {
  const b = bucketOf(f);
  if (!b) return null;
  const t = typeof f.failure_type === 'string' ? f.failure_type : String(f.failure_type);
  return {
    t0: b.t0, size: b.size, kind: 'gap',
    type: f.failure_type, label: humanizeWords(t), family: 'gap',
    zone: '', conf: null,
    details: typeof f.details === 'string' ? f.details : '',
  };
}

function recordFromSystem(rec, recordType) {
  const b = bucketOf(rec);
  if (!b) return null;
  const kind = recordType === 'heartbeat' ? 'heartbeat' : 'system';
  return {
    t0: b.t0, size: b.size, kind,
    type: recordType, label: humanizeWords(recordType), family: 'other',
    zone: '', conf: null, details: '',
  };
}

// Sealed-event ledger entries (full evidence envelope). Every parseable
// record is represented — events, declared gaps, and the system-trace
// records (key rotation, heartbeat, lifecycle) the ledger also carries.
// Unparseable payloads are counted, never silently vanished.
function normalizeEnvelope(envelope) {
  const entries = (envelope && envelope.ledgers && envelope.ledgers.sealed_events
    && envelope.ledgers.sealed_events.entries) || [];
  const records = [];
  let unparsed = 0;
  for (const e of entries) {
    let rec;
    try { rec = JSON.parse(e.payload_json); } catch { unparsed += 1; continue; }
    let r = null;
    if (rec.record_type === 'failure' || (rec.record_type === undefined && rec.failure_type !== undefined)) {
      r = recordFromFailure(rec);
    } else if (rec.record_type === 'event' || (rec.record_type === undefined && rec.event_type !== undefined)) {
      r = recordFromEvent(rec);
    } else if (typeof rec.record_type === 'string') {
      r = recordFromSystem(rec, rec.record_type);
    }
    if (r) records.push(r); else unparsed += 1;
  }
  return { records: sortRecords(records), unparsed };
}

// Event-export artifact (ExportBundle from `export_events`) — events and
// declared gaps only; system-trace records are excluded from exports.
function normalizeBundle(bundle) {
  const records = [];
  let unparsed = 0;
  const batches = (bundle && bundle.artifact && bundle.artifact.batches) || [];
  for (const batch of batches) {
    for (const bucket of batch.buckets || []) {
      for (const e of bucket.events || []) {
        const r = recordFromEvent(e);
        if (r) records.push(r); else unparsed += 1;
      }
      for (const f of bucket.failures || []) {
        const r = recordFromFailure(f);
        if (r) records.push(r); else unparsed += 1;
      }
    }
  }
  return { records: sortRecords(records), unparsed };
}

// Stable time sort: buckets can repeat, and within a bucket the sealed order
// is preserved (Array.prototype.sort is stable per spec).
function sortRecords(records) {
  return records.slice().sort((a, b) => a.t0 - b.t0);
}

// ---- fold segmentation (Sublime's code folding, for quiet time) ---------

const DEFAULT_BUCKET_S = 600;
const FOLD_MIN_QUIET_S = 3600; // fold only when at least an hour has nothing to say
const FOLD_PAD_S = 600;        // one bucket of breathing room beside real records

// Split the covered window into segments: 'span' (drawn proportional to
// time) and 'fold' (a fixed-height pleat standing in for quiet). Anchors are
// the records that must stay at readable scale — events, gaps, and system
// records. Heartbeats deliberately do NOT anchor: a night of hourly
// heartbeats still folds, and the fold reports how many were sealed inside
// it ("quiet, with proof of watching"). A gap (failure record) is an anchor,
// so a declared blind spot can never be folded out of sight.
function buildSegments(records, opts) {
  const o = opts || {};
  const minQuiet = o.foldMinQuietS || FOLD_MIN_QUIET_S;
  const pad = o.foldPadS !== undefined ? o.foldPadS : FOLD_PAD_S;
  const anchors = records.filter((r) => r.kind !== 'heartbeat');
  const beats = records.filter((r) => r.kind === 'heartbeat');
  if (!anchors.length && !beats.length) return [];
  const base = anchors.length ? anchors : beats; // heartbeat-only ledgers still render
  let cov0 = typeof o.coverageT0 === 'number' ? o.coverageT0 : base[0].t0;
  let cov1 = typeof o.coverageT1 === 'number' ? o.coverageT1
    : base.reduce((m, r) => Math.max(m, r.t0 + r.size), 0);
  cov0 = Math.min(cov0, base[0].t0);
  cov1 = Math.max(cov1, base.reduce((m, r) => Math.max(m, r.t0 + r.size), 0));

  // Quiet boundaries: walk anchor bucket ends against next anchor starts,
  // including the coverage edges (a week of leading quiet folds too).
  const segments = [];
  let cursor = cov0;
  let prevEnd = cov0;
  const anchorList = anchors.length ? anchors : beats;
  for (let i = 0; i <= anchorList.length; i++) {
    const nextStart = i < anchorList.length ? anchorList[i].t0 : cov1;
    const leadingEdge = prevEnd === cov0 && cursor === cov0;
    const trailingEdge = i === anchorList.length;
    const padL = leadingEdge ? 0 : pad;   // no breathing room needed against a window edge
    const padR = trailingEdge ? 0 : pad;
    const quiet = nextStart - prevEnd;
    if (quiet >= minQuiet + padL + padR) {
      if (prevEnd + padL > cursor) segments.push({ kind: 'span', t0: cursor, t1: prevEnd + padL });
      segments.push({ kind: 'fold', t0: prevEnd + padL, t1: nextStart - padR });
      cursor = nextStart - padR;
    }
    if (i < anchorList.length) prevEnd = Math.max(prevEnd, anchorList[i].t0 + anchorList[i].size);
  }
  if (cursor < cov1 || !segments.length) segments.push({ kind: 'span', t0: cursor, t1: Math.max(cov1, cursor + 1) });

  // Count the heartbeats each fold quietly contains.
  for (const s of segments) {
    if (s.kind === 'fold') s.heartbeats = beats.filter((b) => b.t0 >= s.t0 && b.t0 < s.t1).length;
  }
  return segments;
}

// ---- vertical layout ----------------------------------------------------

const FOLD_PX = 18;      // fixed pleat height
const MIN_SPAN_PX = 6;   // a span never collapses to invisibility

// Distribute heightPx over the segments: folds get a fixed pleat, spans share
// the rest proportionally to their duration. Returns segments with y0/y1.
function layoutSegments(segments, heightPx, opts) {
  const o = opts || {};
  const foldPx = o.foldPx || FOLD_PX;
  const minSpanPx = o.minSpanPx || MIN_SPAN_PX;
  const folds = segments.filter((s) => s.kind === 'fold').length;
  const spans = segments.filter((s) => s.kind === 'span');
  const spanDur = spans.reduce((d, s) => d + (s.t1 - s.t0), 0) || 1;
  const avail = Math.max(heightPx - folds * foldPx - spans.length * minSpanPx, 0);
  let y = 0;
  return segments.map((s) => {
    const h = s.kind === 'fold' ? foldPx : minSpanPx + ((s.t1 - s.t0) / spanDur) * avail;
    const out = { kind: s.kind, t0: s.t0, t1: s.t1, y0: y, y1: y + h, heartbeats: s.heartbeats || 0 };
    y += h;
    return out;
  });
}

// Time -> pixel through the folded coordinate space. Clamps to the domain.
function yOfTime(layout, t) {
  if (!layout.length) return 0;
  if (t <= layout[0].t0) return layout[0].y0;
  for (const s of layout) {
    if (t <= s.t1) {
      const f = (t - s.t0) / Math.max(s.t1 - s.t0, 1);
      return s.y0 + f * (s.y1 - s.y0);
    }
  }
  return layout[layout.length - 1].y1;
}

// Pixel -> time, the inverse of yOfTime (same clamping).
//
// Returns a WHOLE second, floored. Two reasons, and both matter: the Swift
// port returns an Int, so a fractional result here would make the two
// implementations disagree on every probe in the parity fixture; and a
// sub-second scrub position is a precision the record does not have. Callers
// snap to the bucket anyway — this just refuses to invent the digits.
function timeOfY(layout, y) {
  if (!layout.length) return 0;
  if (y <= layout[0].y0) return layout[0].t0;
  for (const s of layout) {
    if (y <= s.y1) {
      const f = (y - s.y0) / Math.max(s.y1 - s.y0, 1e-9);
      return s.t0 + Math.floor((s.t1 - s.t0) * f);
    }
  }
  return layout[layout.length - 1].t1;
}

// Ruler lines for the minimap: the smallest interval whose lines stay at
// least minGapPx apart inside every span. Day boundaries are marked major.
const GRID_STEPS_S = [600, 1800, 3600, 3 * 3600, 6 * 3600, 12 * 3600, 86400];
// A ruler denser than this is not a ruler. Without the cap, a multi-year
// export falls past every candidate step (none can satisfy the minimum gap at
// that scale), lands on the largest, and emits one line per day — thousands of
// DOM nodes for a drawing 84 pixels wide.
const MAX_GRID_LINES = 240;

function gridLines(layout, minGapPx) {
  const spans = layout.filter((s) => s.kind === 'span');
  if (!spans.length) return [];
  let step = GRID_STEPS_S[GRID_STEPS_S.length - 1];
  for (const cand of GRID_STEPS_S) {
    const ok = spans.every((s) => {
      const pxPerS = (s.y1 - s.y0) / Math.max(s.t1 - s.t0, 1);
      return cand * pxPerS >= minGapPx || s.t1 - s.t0 < cand;
    });
    if (ok) { step = cand; break; }
  }
  // Past the largest candidate the only way to stay readable is a coarser
  // step, so keep doubling until the whole ruler fits under the cap.
  const spanTotal = spans.reduce((d, s) => d + (s.t1 - s.t0), 0);
  while (step > 0 && spanTotal / step > MAX_GRID_LINES) step *= 2;
  const lines = [];
  for (const s of spans) {
    for (let t = Math.ceil(s.t0 / step) * step; t <= s.t1; t += step) {
      lines.push({ t, y: yOfTime(layout, t), major: t % 86400 === 0 });
    }
  }
  return lines;
}

// ---- day grouping (the sticky headers and their density strips) ---------

const DAY_S = 86400;

// One entry per UTC day that has visible records. `cells` holds only lit
// buckets: { i (bucket index in the day), count, family, hasGap } — family
// is the bucket's dominant family, with the reserved statuses (gap, then
// tamper) always winning, so trouble is never averaged away. Heartbeats do
// not light cells (routine proof-of-life is not activity). `cov0`/`cov1`
// clip each day's strip to the covered window: hours the export does not
// cover render as absent, never as "quiet" (no guessed history).
function buildDays(records, opts) {
  const o = opts || {};
  const bucketS = o.bucketS || DEFAULT_BUCKET_S;
  const perDay = Math.max(1, Math.round(DAY_S / bucketS));
  const days = new Map();
  const visible = records.filter((r) => r.kind !== 'heartbeat');
  visible.forEach((r, index) => {
    const dayT0 = Math.floor(r.t0 / DAY_S) * DAY_S;
    let day = days.get(dayT0);
    if (!day) {
      day = { dayT0, count: 0, gapCount: 0, tamperCount: 0, worstSeverity: null, cellMap: new Map(), firstIndex: index };
      days.set(dayT0, day);
    }
    day.count += 1;
    if (r.kind === 'gap') day.gapCount += 1;
    if (r.family === 'tamper') day.tamperCount += 1;
    if (typeof r.severity === 'number') {
      day.worstSeverity = Math.max(day.worstSeverity === null ? -1 : day.worstSeverity, r.severity);
    }
    const i = Math.min(Math.floor((r.t0 - dayT0) / bucketS), perDay - 1);
    let cell = day.cellMap.get(i);
    if (!cell) { cell = { i, count: 0, families: {}, hasGap: false, worstSeverity: null }; day.cellMap.set(i, cell); }
    cell.count += 1;
    cell.families[r.family] = (cell.families[r.family] || 0) + 1;
    if (r.kind === 'gap') cell.hasGap = true;
    // Worst-first: a cell reports the most serious thing in it, never an
    // average. Null on surfaces whose records carry no severity at all (the
    // evidence viewer's sealed events have no severity field).
    if (typeof r.severity === 'number') {
      cell.worstSeverity = Math.max(cell.worstSeverity === null ? -1 : cell.worstSeverity, r.severity);
    }
  });
  const cov0 = typeof o.coverageT0 === 'number' ? o.coverageT0 : (visible.length ? visible[0].t0 : 0);
  const cov1 = typeof o.coverageT1 === 'number' ? o.coverageT1
    : visible.reduce((m, r) => Math.max(m, r.t0 + r.size), cov0);
  return Array.from(days.values()).sort((a, b) => a.dayT0 - b.dayT0).map((day) => ({
    dayT0: day.dayT0,
    label: fmtDayLabel(day.dayT0),
    count: day.count,
    gapCount: day.gapCount,
    tamperCount: day.tamperCount,
    worstSeverity: day.worstSeverity,
    cellsPerDay: perDay,
    firstIndex: day.firstIndex,
    // The covered part of this day, as cell fractions 0..1 of the strip.
    covFrom: Math.min(Math.max((cov0 - day.dayT0) / DAY_S, 0), 1),
    covTo: Math.min(Math.max((cov1 - day.dayT0) / DAY_S, 0), 1),
    cells: Array.from(day.cellMap.values()).sort((a, b) => a.i - b.i).map((c) => ({
      i: c.i, count: c.count, family: dominantFamily(c), hasGap: c.hasGap,
      worstSeverity: c.worstSeverity,
    })),
  }));
}

// The order ties resolve in. Iterating the object's own keys would resolve a
// tie by insertion order, which is the order the families happened to appear
// in that bucket — the Swift port iterates its `allCases` instead, so the two
// would silently disagree on any bucket with two equally-common families.
const FAMILY_ORDER = ['move', 'touch', 'sound', 'tamper', 'gap', 'other'];

function dominantFamily(cell) {
  if (cell.hasGap) return 'gap';
  if (cell.families.tamper) return 'tamper';
  let best = 'other', n = 0;
  for (const fam of FAMILY_ORDER) {
    const c = cell.families[fam];
    if (c !== undefined && c > n) { n = c; best = fam; }
  }
  return best;
}

// ---- the reading-pane item stream ---------------------------------------

// Interleaves day headers and fold rows with the records, in reading order.
// A fold that crosses midnight is emitted before the new day's header, so an
// overnight quiet reads: …last event · fold pleat · new day · first event.
// Heartbeats that fall inside a fold are summarized by the fold, not listed.
function buildItems(records, segments, days) {
  const items = [];
  const dayByT0 = new Map(days.map((d) => [d.dayT0, d]));
  const folds = segments.filter((s) => s.kind === 'fold');
  const inFold = (t) => folds.some((f) => t >= f.t0 && t < f.t1);
  let foldIdx = 0;
  let curDay = null;
  records.forEach((r, index) => {
    if (r.kind === 'heartbeat' && inFold(r.t0)) return; // counted by the fold row
    while (foldIdx < folds.length && folds[foldIdx].t0 <= r.t0) {
      const f = folds[foldIdx++];
      items.push({ kind: 'fold', t0: f.t0, t1: f.t1, quietS: f.t1 - f.t0, heartbeats: f.heartbeats || 0 });
    }
    const dayT0 = Math.floor(r.t0 / DAY_S) * DAY_S;
    if (dayT0 !== curDay) {
      const day = dayByT0.get(dayT0);
      // Only advance the day cursor when a header is actually emitted — a
      // heartbeat-only stretch must not swallow the header of the visible
      // record that follows it on the same day.
      if (day) { items.push({ kind: 'day', day }); curDay = dayT0; }
    }
    items.push({ kind: r.kind, record: r, index });
  });
  // Trailing quiet (a fold that ends the covered window).
  while (foldIdx < folds.length) {
    const f = folds[foldIdx++];
    items.push({ kind: 'fold', t0: f.t0, t1: f.t1, quietS: f.t1 - f.t0, heartbeats: f.heartbeats || 0 });
  }
  return items;
}

// ---- one call for the whole model ---------------------------------------

function buildTimelineModel(input, opts) {
  const o = opts || {};
  const records = input.records || [];
  const bucketS = o.bucketS || mostCommonSize(records) || DEFAULT_BUCKET_S;
  const segOpts = { coverageT0: o.coverageT0, coverageT1: o.coverageT1, foldMinQuietS: o.foldMinQuietS, foldPadS: o.foldPadS };
  const segments = buildSegments(records, segOpts);
  const days = buildDays(records, { bucketS, coverageT0: o.coverageT0, coverageT1: o.coverageT1 });
  const items = buildItems(records, segments, days);
  const zones = [];
  for (const r of records) {
    if (r.kind === 'event' && r.zone && !zones.includes(r.zone)) zones.push(r.zone);
  }
  const counts = {};
  for (const r of records) {
    if (r.kind === 'heartbeat') continue;
    counts[r.family] = (counts[r.family] || 0) + 1;
  }
  const heartbeats = records.filter((r) => r.kind === 'heartbeat').length;
  return { records, segments, days, items, zones, counts, heartbeats, bucketS, unparsed: input.unparsed || 0 };
}

// Ascending size order, so a tie resolves to the smaller bucket in both
// languages rather than to whichever the Map happened to see first.
function mostCommonSize(records) {
  const freq = new Map();
  for (const r of records) freq.set(r.size, (freq.get(r.size) || 0) + 1);
  let best = 0, n = 0;
  for (const size of Array.from(freq.keys()).sort((a, b) => a - b)) {
    const count = freq.get(size);
    if (count > n) { n = count; best = size; }
  }
  return best;
}

// ---- greeked text (the Sublime minimap trick) ---------------------------

// A tick's width comes from its label's length — the alert is drawn as an
// unreadable-but-recognizable line of "text", so a day of alerts has the
// shape of a page. Clamped so the minimap column stays composed.
function greekWidth(label) {
  const len = typeof label === 'string' ? label.length : 0;
  return Math.max(12, Math.min(46, Math.round(10 + len * 1.3)));
}

// Same zone -> same indent, in order of first appearance: recurring places
// line up into columns, the way indentation shapes code in a minimap.
function zoneIndent(zones, zone) {
  const i = zones.indexOf(zone);
  return i < 0 ? 0 : Math.min(i, 4) * 6;
}

// ---- deterministic UTC formatting (locale-independent, testable) --------

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July',
  'August', 'September', 'October', 'November', 'December'];
const WEEKDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];

function pad2(n) { return n < 10 ? '0' + n : String(n); }

function fmtHM(epochS) {
  const d = new Date(epochS * 1000);
  return pad2(d.getUTCHours()) + ':' + pad2(d.getUTCMinutes());
}

// "06:40 – 06:50" for a bucket — the honest range, never a fake instant.
function fmtBucketRange(t0, sizeS) {
  return fmtHM(t0) + ' – ' + fmtHM(t0 + (sizeS || DEFAULT_BUCKET_S));
}

function fmtDayLabel(dayT0) {
  const d = new Date(dayT0 * 1000);
  return WEEKDAYS[d.getUTCDay()] + ', ' + MONTHS[d.getUTCMonth()] + ' ' + d.getUTCDate() + ', ' + d.getUTCFullYear();
}

function fmtDuration(s) {
  if (s >= DAY_S) {
    const days = Math.floor(s / DAY_S), h = Math.round((s % DAY_S) / 3600);
    return h ? days + ' d ' + h + ' h' : days + ' d';
  }
  if (s >= 3600) {
    const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
    return m ? h + ' h ' + m + ' m' : h + ' h';
  }
  return Math.max(1, Math.round(s / 60)) + ' m';
}

// ---- exports ------------------------------------------------------------

// NOTE: the built viewer inlines this file and verify_core.js into the SAME
// global script scope, so every top-level binding here must be unique across
// both (viewer/build.mjs fails the build if two inlined cores collide).
const timelineApi = {
  EVENT_TYPE_META,
  FAMILY_LABELS,
  normalizeTypeId,
  typeMeta,
  humanizeWords,
  normalizeEnvelope,
  normalizeBundle,
  buildSegments,
  layoutSegments,
  yOfTime,
  timeOfY,
  gridLines,
  buildDays,
  buildItems,
  buildTimelineModel,
  greekWidth,
  zoneIndent,
  fmtHM,
  fmtBucketRange,
  fmtDayLabel,
  fmtDuration,
  DAY_S,
  DEFAULT_BUCKET_S,
};

if (typeof module !== 'undefined' && module.exports) module.exports = timelineApi;
if (typeof globalThis !== 'undefined') globalThis.SecuraCVTimeline = timelineApi;
