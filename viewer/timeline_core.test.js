'use strict';

// Tests for the timeline scrub-view model (the pure data-shaping half of the
// viewer's "day shape" timeline).
//
//   node --test viewer/timeline_core.test.js
//
// The vocabulary tests run against spec/witness_dictionary.json itself, so a
// dictionary change that isn't mirrored here fails loudly (in addition to the
// scripts/lint_dictionary_sync.py CI gate).

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const T = require('./timeline_core');

const load = (p) => JSON.parse(fs.readFileSync(path.join(__dirname, '..', p), 'utf8'));

// Synthetic record helper — a 600 s bucket at t0.
const ev = (t0, extra) => Object.assign({
  t0, size: 600, kind: 'event', type: 'BoundaryCrossingObjectLarge',
  label: 'Large object crossed boundary', family: 'move', zone: 'zone:a', conf: 0.9, details: '',
}, extra);
const beat = (t0) => ({ t0, size: 600, kind: 'heartbeat', type: 'heartbeat', label: 'Heartbeat', family: 'other', zone: '', conf: null, details: '' });

describe('vocabulary mirror', () => {
  const dict = load('spec/witness_dictionary.json');

  it('covers every dictionary event type, exactly', () => {
    const ids = dict.event_types.items.map((i) => i.id).sort();
    assert.deepEqual(Object.keys(T.EVENT_TYPE_META).sort(), ids);
  });

  it('uses the canonical label for every event type', () => {
    for (const item of dict.event_types.items) {
      assert.equal(T.EVENT_TYPE_META[item.id].label, item.label, item.id);
    }
  });

  it('resolves the Rust wire form (PascalCase) to the same meta as the id', () => {
    for (const item of dict.event_types.items) {
      const fromWire = T.typeMeta(item.rust_variant);
      const fromId = T.typeMeta(item.id);
      assert.deepEqual(fromWire, fromId, item.id);
      assert.equal(fromWire.label, item.label);
    }
  });

  it('assigns every event type a family the legend knows', () => {
    for (const id of Object.keys(T.EVENT_TYPE_META)) {
      const fam = T.EVENT_TYPE_META[id].family;
      assert.ok(Object.prototype.hasOwnProperty.call(T.FAMILY_LABELS, fam), id + ' -> ' + fam);
    }
  });

  it('keeps unknown/future types honest: generic label, neutral family', () => {
    const m = T.typeMeta('SomeFutureSignal');
    assert.equal(m.family, 'other');
    assert.equal(m.label, 'Some future signal');
  });

  it('humanizes failure types generically (no vocabulary copy to drift)', () => {
    for (const v of dict.failure_types.rust_variants) {
      const label = T.humanizeWords(v);
      assert.match(label, /^[A-Z][a-z]/);
      assert.ok(!label.includes('_'), label);
    }
    assert.equal(T.humanizeWords('StorageFull'), 'Storage full');
    assert.equal(T.humanizeWords('GapMissingData'), 'Gap missing data');
  });
});

describe('record normalization', () => {
  it('normalizes the envelope fixture (3 sealed events)', () => {
    const { records, unparsed } = T.normalizeEnvelope(load('tests/fixtures/envelope/valid_envelope.json'));
    assert.equal(unparsed, 0);
    assert.equal(records.length, 3);
    for (const r of records) {
      assert.equal(r.kind, 'event');
      assert.equal(r.family, 'move');
      assert.equal(r.label, 'Large object crossed boundary');
      assert.equal(r.size, 600);
      assert.equal(typeof r.conf, 'number');
    }
    assert.deepEqual(records.map((r) => r.t0), [600, 1200, 1800]);
    assert.deepEqual(records.map((r) => r.zone), ['zone:a', 'zone:b', 'zone:a']);
  });

  it('normalizes the export-bundle fixture', () => {
    const { records, unparsed } = T.normalizeBundle(load('tests/fixtures/export_bundle/valid_bundle.json'));
    assert.equal(unparsed, 0);
    assert.equal(records.length, 3);
    assert.ok(records.every((r) => r.kind === 'event'));
  });

  it('classifies failures as gaps and system-trace records by their tag', () => {
    const mk = (payload) => ({ payload_json: JSON.stringify(payload) });
    const env = { ledgers: { sealed_events: { entries: [
      mk({ record_type: 'event', event_type: 'TamperDetected', time_bucket: { start_epoch_s: 600, size_s: 600 }, zone_id: 'zone:a', confidence: 1 }),
      mk({ record_type: 'failure', failure_type: 'PowerLoss', time_bucket: { start_epoch_s: 1200, size_s: 600 }, details: 'battery died' }),
      mk({ record_type: 'key_rotation', time_bucket: { start_epoch_s: 1800, size_s: 600 } }),
      mk({ record_type: 'heartbeat', time_bucket: { start_epoch_s: 2400, size_s: 600 } }),
      mk({ record_type: 'lifecycle', time_bucket: { start_epoch_s: 3000, size_s: 600 } }),
      // Legacy, untagged shapes still classify by their distinguishing field.
      mk({ event_type: 'ContactStateChange', time_bucket: { start_epoch_s: 3600, size_s: 600 }, zone_id: 'zone:d', confidence: 0.5 }),
      mk({ failure_type: 'ClockSkew', time_bucket: { start_epoch_s: 4200, size_s: 600 } }),
    ] } } };
    const { records, unparsed } = T.normalizeEnvelope(env);
    assert.equal(unparsed, 0);
    assert.deepEqual(records.map((r) => r.kind),
      ['event', 'gap', 'system', 'heartbeat', 'system', 'event', 'gap']);
    assert.equal(records[0].family, 'tamper');
    assert.equal(records[1].label, 'Power loss');
    assert.equal(records[1].details, 'battery died');
    assert.equal(records[2].label, 'Key rotation');
    assert.equal(records[4].label, 'Lifecycle');
  });

  it('survives payloads that parse but are not records', () => {
    // `null`, a bare number, an array and a string all survive JSON.parse and
    // then throw on the first property read — one bad entry used to take the
    // whole timeline down rather than just itself.
    const env = { ledgers: { sealed_events: { entries: [
      { payload_json: 'null' },
      { payload_json: '5' },
      { payload_json: '[]' },
      { payload_json: '"a string"' },
      { payload_json: 'true' },
      { payload_json: JSON.stringify({ record_type: 'event', event_type: 'ContactStateChange', time_bucket: { start_epoch_s: 600, size_s: 600 } }) },
    ] } } };
    const { records, unparsed } = T.normalizeEnvelope(env);
    assert.equal(records.length, 1, 'the one good record still renders');
    assert.equal(unparsed, 5);
  });

  it('counts a record with no usable time bucket as unparseable', () => {
    const env = { ledgers: { sealed_events: { entries: [
      { payload_json: JSON.stringify({ record_type: 'event', event_type: 'ContactStateChange' }) },
      { payload_json: JSON.stringify({ record_type: 'event', event_type: 'ContactStateChange', time_bucket: { size_s: 600 } }) },
    ] } } };
    const { records, unparsed } = T.normalizeEnvelope(env);
    assert.equal(records.length, 0);
    assert.equal(unparsed, 2);
  });

  it('counts unparseable payloads instead of silently dropping them', () => {
    const env = { ledgers: { sealed_events: { entries: [
      { payload_json: 'not json' },
      { payload_json: JSON.stringify({ record_type: 'event', event_type: 'ContactStateChange', time_bucket: { start_epoch_s: 600, size_s: 600 } }) },
    ] } } };
    const { records, unparsed } = T.normalizeEnvelope(env);
    assert.equal(records.length, 1);
    assert.equal(unparsed, 1);
  });
});

describe('fold segmentation', () => {
  it('does not fold short quiet stretches', () => {
    const segs = T.buildSegments([ev(600), ev(1800), ev(3600)]);
    assert.deepEqual(segs.map((s) => s.kind), ['span']);
  });

  it('folds an hour-plus of quiet, with one bucket of padding each side', () => {
    const segs = T.buildSegments([ev(600), ev(600 + 8 * 3600)]);
    assert.deepEqual(segs.map((s) => s.kind), ['span', 'fold', 'span']);
    const fold = segs[1];
    assert.equal(fold.t0, 1200 + 600);       // bucket end + pad
    assert.equal(fold.t1, 600 + 8 * 3600 - 600); // next bucket start - pad
  });

  it('never folds over a declared gap — a blind spot is an anchor', () => {
    const withGap = [ev(600), ev(600, { t0: 600 + 8 * 3600 })];
    withGap.splice(1, 0, { t0: 600 + 4 * 3600, size: 600, kind: 'gap', type: 'PowerLoss', label: 'Power loss', family: 'gap', zone: '', conf: null, details: '' });
    const segs = T.buildSegments(withGap);
    for (const s of segs.filter((x) => x.kind === 'fold')) {
      assert.ok(s.t1 <= 600 + 4 * 3600 || s.t0 >= 600 + 4 * 3600 + 600,
        'fold must not contain the gap record');
    }
  });

  it('folds across heartbeats and counts them as proof of watching', () => {
    const records = [ev(600), beat(600 + 2 * 3600), beat(600 + 4 * 3600), ev(600 + 8 * 3600)];
    const segs = T.buildSegments(records);
    const folds = segs.filter((s) => s.kind === 'fold');
    assert.equal(folds.length, 1);
    assert.equal(folds[0].heartbeats, 2);
  });

  it('folds leading and trailing quiet against a wider coverage window', () => {
    const segs = T.buildSegments([ev(12 * 3600)], { coverageT0: 0, coverageT1: 86400 });
    assert.deepEqual(segs.map((s) => s.kind), ['fold', 'span', 'fold']);
    assert.equal(segs[0].t0, 0);
    assert.equal(segs[2].t1, 86400);
  });
});

describe('layout mapping', () => {
  const records = [ev(600), ev(600 + 12 * 3600)];
  const segs = T.buildSegments(records);
  const layout = T.layoutSegments(segs, 480);

  it('fills the requested height exactly', () => {
    assert.ok(Math.abs(layout[layout.length - 1].y1 - 480) < 1e-6);
  });

  it('gives folds their fixed pleat height', () => {
    const fold = layout.find((s) => s.kind === 'fold');
    assert.ok(fold);
    assert.equal(Math.round(fold.y1 - fold.y0), 18);
  });

  it('yOfTime and timeOfY are monotonic inverses over the domain', () => {
    const t0 = layout[0].t0, t1 = layout[layout.length - 1].t1;
    let lastY = -1;
    for (let i = 0; i <= 40; i++) {
      const t = t0 + ((t1 - t0) * i) / 40;
      const y = T.yOfTime(layout, t);
      assert.ok(y >= lastY - 1e-9, 'monotonic');
      lastY = y;
      const back = T.timeOfY(layout, y);
      assert.ok(Math.abs(back - t) < 60, 'round-trips within a minute');
    }
  });

  it('returns whole seconds, so the Swift port can agree and no sub-second precision is invented', () => {
    for (let i = 0; i <= 20; i++) {
      const t = T.timeOfY(layout, (480 * i) / 20);
      assert.equal(t, Math.trunc(t), 'timeOfY must return an integer second');
    }
  });

  it('clamps outside the domain', () => {
    assert.equal(T.yOfTime(layout, -1e9), layout[0].y0);
    assert.equal(T.yOfTime(layout, 1e12), layout[layout.length - 1].y1);
  });

  it('caps the ruler on a huge span instead of drawing one line per day', () => {
    // A multi-year export: no candidate step can satisfy the minimum gap at
    // that scale, so without the cap this emitted thousands of DOM nodes.
    const year = 365 * 86400;
    const wide = T.layoutSegments(T.buildSegments([ev(0), ev(6 * year)]), 480);
    const lines = T.gridLines(wide, 22);
    assert.ok(lines.length <= 240, 'got ' + lines.length + ' grid lines');
    assert.ok(lines.length > 0, 'a ruler should still be drawn');
  });

  it('chooses a grid step that keeps lines apart', () => {
    const lines = T.gridLines(layout, 22);
    assert.ok(lines.length > 0);
    const ys = lines.map((l) => l.y).sort((a, b) => a - b);
    for (let i = 1; i < ys.length; i++) {
      // Lines in different spans can sit close to a fold edge; within a span
      // the chosen step must hold the minimum gap.
      const a = lines.find((l) => l.y === ys[i - 1]);
      const b = lines.find((l) => l.y === ys[i]);
      const sameSpan = layout.some((s) => s.kind === 'span'
        && a.t >= s.t0 && a.t <= s.t1 && b.t >= s.t0 && b.t <= s.t1);
      if (sameSpan) assert.ok(ys[i] - ys[i - 1] >= 21.9, `${ys[i - 1]} .. ${ys[i]}`);
    }
  });
});

describe('day grouping', () => {
  it('groups by UTC day with worst-status-wins cells', () => {
    const records = [
      ev(600), ev(660), // same bucket, count 2
      ev(600 + 86400, { type: 'TamperDetected', label: 'Tamper detected', family: 'tamper' }),
      { t0: 1200 + 86400, size: 600, kind: 'gap', type: 'PowerLoss', label: 'Power loss', family: 'gap', zone: '', conf: null, details: '' },
    ];
    const days = T.buildDays(records, { bucketS: 600 });
    assert.equal(days.length, 2);
    assert.equal(days[0].count, 2);
    assert.equal(days[0].cells.length, 1);
    assert.equal(days[0].cells[0].count, 2);
    assert.equal(days[0].cells[0].family, 'move');
    assert.equal(days[1].tamperCount, 1);
    assert.equal(days[1].gapCount, 1);
    assert.equal(days[1].cells.find((c) => c.hasGap).family, 'gap');
    assert.equal(days[1].cells.find((c) => !c.hasGap).family, 'tamper');
  });

  it('clips each day strip to the covered window (no guessed history)', () => {
    const days = T.buildDays([ev(6 * 3600)], { bucketS: 600, coverageT0: 4 * 3600, coverageT1: 10 * 3600 });
    assert.equal(days.length, 1);
    assert.ok(Math.abs(days[0].covFrom - 4 / 24) < 1e-9);
    assert.ok(Math.abs(days[0].covTo - 10 / 24) < 1e-9);
  });

  it('does not light cells for heartbeats', () => {
    const days = T.buildDays([ev(600), beat(1200)], { bucketS: 600 });
    assert.equal(days[0].count, 1);
    assert.equal(days[0].cells.length, 1);
  });
});

describe('item stream', () => {
  it('interleaves day headers and folds in reading order', () => {
    const records = [ev(600), ev(600 + 30 * 3600)]; // next day, big quiet between
    const model = T.buildTimelineModel({ records });
    const kinds = model.items.map((i) => i.kind);
    assert.deepEqual(kinds, ['day', 'event', 'fold', 'day', 'event']);
  });

  it('summarizes folded heartbeats instead of listing them', () => {
    const records = [ev(600), beat(600 + 3 * 3600), ev(600 + 30 * 3600)];
    const model = T.buildTimelineModel({ records });
    const fold = model.items.find((i) => i.kind === 'fold');
    assert.equal(fold.heartbeats, 1);
    assert.ok(!model.items.some((i) => i.kind === 'heartbeat'));
  });

  it('lists a heartbeat that falls inside an active span', () => {
    const records = [ev(600), beat(1200), ev(1800)];
    const model = T.buildTimelineModel({ records });
    assert.ok(model.items.some((i) => i.kind === 'heartbeat'));
  });
});

describe('model summary', () => {
  it('counts by family, excluding heartbeats, and tallies zones in first-appearance order', () => {
    const records = [
      ev(600, { zone: 'zone:b' }),
      ev(1200, { zone: 'zone:a' }),
      ev(1800, { zone: 'zone:b' }),
      beat(2400),
    ];
    const model = T.buildTimelineModel({ records });
    assert.deepEqual(model.zones, ['zone:b', 'zone:a']);
    assert.equal(model.counts.move, 3);
    assert.equal(model.heartbeats, 1);
    assert.equal(T.zoneIndent(model.zones, 'zone:b'), 0);
    assert.equal(T.zoneIndent(model.zones, 'zone:a'), 6);
    assert.equal(T.zoneIndent(model.zones, 'zone:none'), 0);
  });
});

// The fixture is generated FROM this implementation, so these assertions are
// a staleness guard: they fail when the model changes and the fixture wasn't
// regenerated — which is exactly when the Swift port would silently diverge.
// The Swift side asserts the same file in TimelineScrubTests.swift.
describe('cross-language parity fixture', () => {
  const fx = load('tests/fixtures/timeline/scrub_parity.json');
  const round = (n) => Math.round(n * 1000) / 1000;

  it('has scenarios with the properties worth pinning', () => {
    const names = fx.scenarios.map((s) => s.name);
    for (const want of ['ordinary-day', 'gap-is-never-folded', 'overnight-heartbeats',
      'tamper-and-system', 'coverage-window', 'single-record']) {
      assert.ok(names.includes(want), 'missing scenario ' + want);
    }
  });

  for (const sc of load('tests/fixtures/timeline/scrub_parity.json').scenarios) {
    it('reproduces the fixture for ' + sc.name, () => {
      const model = T.buildTimelineModel({ records: sc.records, unparsed: 0 }, {
        coverageT0: sc.coverage_t0 === null ? undefined : sc.coverage_t0,
        coverageT1: sc.coverage_t1 === null ? undefined : sc.coverage_t1,
      });
      const layout = T.layoutSegments(model.segments, fx.layout_height);
      const e = sc.expected;

      // Assert the field is PRESENT, not merely equal: reading a misspelled
      // property gave undefined on both sides and passed silently.
      assert.equal(typeof e.bucket_seconds, 'number', 'fixture must carry bucket_seconds');
      assert.equal(model.bucketS, e.bucket_seconds);
      assert.deepEqual(model.counts, e.counts);
      assert.equal(model.heartbeats, e.heartbeats);
      assert.deepEqual(model.zones, e.zones);
      assert.deepEqual(model.segments.map((s) => ({
        kind: s.kind, t0: s.t0, t1: s.t1, heartbeats: s.heartbeats || 0,
      })), e.segments);
      assert.deepEqual(layout.map((s) => ({
        kind: s.kind, t0: s.t0, t1: s.t1, y0: round(s.y0), y1: round(s.y1),
      })), e.layout);
      assert.deepEqual(T.gridLines(layout, fx.grid_min_gap).map((l) => ({
        t: l.t, y: round(l.y), major: l.major,
      })), e.grid_lines);
      assert.deepEqual(model.days.map((d) => ({
        day_t0: d.dayT0, label: d.label, count: d.count, gap_count: d.gapCount,
        tamper_count: d.tamperCount, cells_per_day: d.cellsPerDay, first_index: d.firstIndex,
        worst_severity: d.worstSeverity === undefined ? null : d.worstSeverity,
        coverage_from: round(d.covFrom), coverage_to: round(d.covTo),
        cells: d.cells.map((c) => ({
          i: c.i, count: c.count, family: c.family, has_gap: c.hasGap,
          worst_severity: c.worstSeverity === undefined ? null : c.worstSeverity,
        })),
      })), e.days);
      assert.deepEqual(model.items.map((it) => {
        if (it.kind === 'day') return { kind: 'day', day_t0: it.day.dayT0 };
        if (it.kind === 'fold') return { kind: 'fold', t0: it.t0, t1: it.t1, heartbeats: it.heartbeats };
        return { kind: it.kind, index: it.index, t0: it.record.t0, label: it.record.label };
      }), e.items);
      for (const p of e.time_probes) assert.equal(round(T.yOfTime(layout, p.t)), p.y, 't=' + p.t);
      for (const p of e.offset_probes) assert.equal(T.timeOfY(layout, p.y), p.t, 'y=' + p.y);
    });
  }

  it('reproduces the pinned formatting vectors', () => {
    const f = fx.formatting;
    for (const [raw, want] of Object.entries(f.humanize)) assert.equal(T.humanizeWords(raw), want, raw);
    for (const v of f.hour_minute) assert.equal(T.fmtHM(v.t), v.s);
    for (const v of f.bucket_range) assert.equal(T.fmtBucketRange(v.t, v.size), v.s);
    for (const v of f.day_label) assert.equal(T.fmtDayLabel(v.t), v.s);
    for (const v of f.duration) assert.equal(T.fmtDuration(v.s), v.out);
    for (const v of f.greek_width) assert.equal(T.greekWidth(v.label), v.w);
    const zones = f.zone_indent.filter((z) => z.zone !== 'zone:missing').map((z) => z.zone);
    for (const v of f.zone_indent) assert.equal(T.zoneIndent(zones, v.zone), v.indent, v.zone);
  });
});

describe('formatting (deterministic UTC)', () => {
  it('formats bucket ranges, day labels, and durations', () => {
    assert.equal(T.fmtHM(600), '00:10');
    assert.equal(T.fmtBucketRange(24000, 600), '06:40 – 06:50');
    assert.equal(T.fmtDayLabel(0), 'Thursday, January 1, 1970');
    assert.equal(T.fmtDuration(600), '10 m');
    assert.equal(T.fmtDuration(3 * 3600 + 20 * 60), '3 h 20 m');
    assert.equal(T.fmtDuration(2 * 86400), '2 d');
  });

  it('clamps greeked tick widths', () => {
    assert.equal(T.greekWidth(''), 12);
    assert.ok(T.greekWidth('Large object crossed boundary') <= 46);
    assert.ok(T.greekWidth('Sound') > 12);
  });
});
