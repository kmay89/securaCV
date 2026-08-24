// Generate the cross-language timeline parity fixture.
//
//   node viewer/tools/gen_timeline_parity.mjs           # write the fixture
//   node viewer/tools/gen_timeline_parity.mjs --check   # fail if it is stale
//
// The fixture pins the timeline scrub model's decisions — how quiet folds,
// where a time lands on the minimap, how a day's density strip is built — as
// plain data, so the JavaScript implementation (viewer/timeline_core.js) and
// the Swift port (ios/Shared/TimelineScrub.swift) can be asserted against the
// SAME expected values. Two surfaces that disagree about the shape of a day
// disagree about what the day WAS, which is exactly what a witness record
// cannot afford.
//
// This file is the generator, so JS is the reference implementation and Swift
// is checked against it. When a rule changes: change both, re-run this, and
// commit the fixture with the code.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createRequire } from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const require = createRequire(import.meta.url);
const T = require(join(here, '..', 'timeline_core.js'));

const OUT = join(repo, 'tests', 'fixtures', 'timeline', 'scrub_parity.json');

// ---- the scenarios ------------------------------------------------------
// Each is a hand-built day with a property worth pinning. Times are UTC epoch
// seconds on 10-minute buckets, the shape the kernel actually seals.

const DAY = 86400;
const H = 3600;
const BASE = 1750809600; // 2025-06-25T00:00:00Z — a Wednesday, fixed forever

const ev = (t0, label, family, zone, conf) => ({
  t0, size: 600, kind: 'event', type: label, label, family,
  zone: zone || '', conf: typeof conf === 'number' ? conf : null, details: '',
});
const gap = (t0, label, details) => ({
  t0, size: 600, kind: 'gap', type: label, label, family: 'gap',
  zone: '', conf: null, details: details || '',
});
const sys = (t0, label) => ({
  t0, size: 600, kind: 'system', type: label, label, family: 'other',
  zone: '', conf: null, details: '',
});
const beat = (t0) => ({
  t0, size: 600, kind: 'heartbeat', type: 'heartbeat', label: 'Heartbeat',
  family: 'other', zone: '', conf: null, details: '',
});

const SCENARIOS = [
  {
    name: 'ordinary-day',
    why: 'A normal day: a few clusters, an hour of quiet that folds, one busy bucket.',
    records: [
      ev(BASE + 6 * H + 2400, 'Small object crossed boundary', 'move', 'zone:garden', 0.72),
      ev(BASE + 7 * H, 'Vehicle arrival/departure', 'move', 'zone:driveway', 0.94),
      ev(BASE + 7 * H + 600, 'Large object crossed boundary', 'move', 'zone:driveway', 0.91),
      ev(BASE + 7 * H + 600, 'Contact state change', 'touch', 'zone:porch', 0.99),
      ev(BASE + 9 * H + 1800, 'Large object crossed boundary', 'move', 'zone:sidewalk', 0.66),
      ev(BASE + 11 * H, 'Acoustic impulse in zone', 'sound', 'zone:garden', 0.58),
    ],
  },
  {
    name: 'gap-is-never-folded',
    why: 'A declared blind spot anchors the layout: quiet may fold around it, never over it.',
    records: [
      ev(BASE + 1 * H, 'Contact state change', 'touch', 'zone:porch', 0.99),
      gap(BASE + 9 * H, 'Power loss', 'mains dropped for one bucket'),
      ev(BASE + 20 * H, 'Vehicle arrival/departure', 'move', 'zone:driveway', 0.9),
    ],
  },
  {
    name: 'overnight-heartbeats',
    why: 'A quiet night still folds; the fold reports the heartbeats sealed inside it.',
    records: [
      ev(BASE + 18 * H, 'Contact state change', 'touch', 'zone:porch', 0.98),
      beat(BASE + 20 * H), beat(BASE + 22 * H), beat(BASE + 24 * H), beat(BASE + 26 * H),
      ev(BASE + 30 * H, 'Small object crossed boundary', 'move', 'zone:garden', 0.6),
    ],
  },
  {
    name: 'tamper-and-system',
    why: 'Reserved statuses win their cell outright, so trouble is never averaged away.',
    records: [
      ev(BASE + 12 * H, 'Large object crossed boundary', 'move', 'zone:gate', 0.8),
      ev(BASE + 12 * H, 'Tamper detected', 'tamper', 'zone:gate', 0.97),
      sys(BASE + 12 * H + 600, 'Key rotation'),
    ],
  },
  {
    name: 'coverage-window',
    why: 'Leading and trailing quiet fold against the export window; uncovered hours stay blank.',
    coverageT0: BASE,
    coverageT1: BASE + DAY,
    records: [ev(BASE + 12 * H, 'Acoustic impulse in zone', 'sound', 'zone:garden', 0.5)],
  },
  {
    name: 'trailing-heartbeats',
    why: 'The domain spans heartbeats recorded after the last anchor; only folding excludes them.',
    records: [
      ev(BASE + 6 * H, 'Contact state change', 'touch', 'zone:porch', 0.9),
      beat(BASE + 10 * H), beat(BASE + 14 * H), beat(BASE + 18 * H),
    ],
  },
  {
    name: 'jittered-outside-window',
    why: 'A jittered event outside the receipt window widens the coverage track to the bucket grid, '
      + 'so no lit cell is ever drawn over a region the same strip calls uncovered.',
    coverageT0: BASE + 6 * H,
    coverageT1: BASE + 18 * H,
    records: [
      ev(BASE + 6 * H - 120, 'Contact state change', 'touch', 'zone:porch', 0.9),
      ev(BASE + 12 * H, 'Vehicle arrival/departure', 'move', 'zone:driveway', 0.8),
      ev(BASE + 18 * H + 90, 'Small object crossed boundary', 'move', 'zone:garden', 0.6),
    ],
  },
  {
    name: 'tie-breaking',
    why: 'Ties must resolve identically in both languages: one bucket with two equally-common '
      + 'families, and a mixed set of bucket sizes with an equal count.',
    records: [
      // Same bucket, one 'touch' and one 'sound' — a 1:1 family tie.
      ev(BASE + 5 * H, 'Contact state change', 'touch', 'zone:porch', 0.9),
      ev(BASE + 5 * H, 'Acoustic impulse in zone', 'sound', 'zone:garden', 0.7),
      // Two 300 s buckets and two 600 s buckets — a tie on the modal size.
      { t0: BASE + 5 * H + 600, size: 300, kind: 'event', type: 'Small object crossed boundary',
        label: 'Small object crossed boundary', family: 'move', zone: 'zone:garden', conf: 0.5, details: '' },
      { t0: BASE + 5 * H + 900, size: 300, kind: 'event', type: 'Small object crossed boundary',
        label: 'Small object crossed boundary', family: 'move', zone: 'zone:garden', conf: 0.5, details: '' },
      ev(BASE + 5 * H + 1200, 'Vehicle arrival/departure', 'move', 'zone:driveway', 0.8),
    ],
  },
  {
    name: 'wide-span-ruler',
    why: 'A years-wide export must still produce a readable ruler, not one line per day.',
    records: [ev(BASE, 'Contact state change', 'touch', 'zone:porch', 1.0),
              ev(BASE + 6 * 365 * DAY, 'Tamper detected', 'tamper', 'zone:gate', 0.99)],
  },
  {
    name: 'single-record',
    why: 'One record must still produce a valid, non-degenerate layout.',
    records: [ev(BASE + 8 * H, 'Contact state change', 'touch', 'zone:porch', 1.0)],
  },
];

// ---- expected values ----------------------------------------------------

const round = (n) => Math.round(n * 1000) / 1000;
const LAYOUT_HEIGHT = 480;

function expectedFor(scenario) {
  const model = T.buildTimelineModel({ records: scenario.records, unparsed: 0 }, {
    coverageT0: scenario.coverageT0,
    coverageT1: scenario.coverageT1,
  });
  const layout = T.layoutSegments(model.segments, LAYOUT_HEIGHT);

  // Probe the time<->offset mapping at fixed fractions rather than dumping
  // every pixel: the samples pin the curve, including inside folds.
  const domain0 = layout.length ? layout[0].t0 : 0;
  const domain1 = layout.length ? layout[layout.length - 1].t1 : 0;
  const probes = [];
  for (let i = 0; i <= 8; i++) {
    const t = domain0 + Math.round(((domain1 - domain0) * i) / 8);
    probes.push({ t, y: round(T.yOfTime(layout, t)) });
  }
  const yProbes = [];
  for (let i = 0; i <= 8; i++) {
    const y = (LAYOUT_HEIGHT * i) / 8;
    yProbes.push({ y: round(y), t: T.timeOfY(layout, y) });
  }

  return {
    bucket_seconds: model.bucketS,
    counts: model.counts,
    heartbeats: model.heartbeats,
    zones: model.zones,
    segments: model.segments.map((s) => ({
      kind: s.kind, t0: s.t0, t1: s.t1, heartbeats: s.heartbeats || 0,
    })),
    layout: layout.map((s) => ({
      kind: s.kind, t0: s.t0, t1: s.t1, y0: round(s.y0), y1: round(s.y1),
    })),
    grid_lines: T.gridLines(layout, 22).map((l) => ({ t: l.t, y: round(l.y), major: l.major })),
    days: model.days.map((d) => ({
      day_t0: d.dayT0, label: d.label, count: d.count, gap_count: d.gapCount,
      tamper_count: d.tamperCount, cells_per_day: d.cellsPerDay, first_index: d.firstIndex,
      worst_severity: d.worstSeverity === undefined ? null : d.worstSeverity,
      coverage_from: round(d.covFrom), coverage_to: round(d.covTo),
      cells: d.cells.map((c) => ({
        i: c.i, count: c.count, family: c.family, has_gap: c.hasGap,
        worst_severity: c.worstSeverity === undefined ? null : c.worstSeverity,
      })),
    })),
    items: model.items.map((it) => {
      if (it.kind === 'day') return { kind: 'day', day_t0: it.day.dayT0 };
      if (it.kind === 'fold') return { kind: 'fold', t0: it.t0, t1: it.t1, heartbeats: it.heartbeats };
      return { kind: it.kind, index: it.index, t0: it.record.t0, label: it.record.label };
    }),
    time_probes: probes,
    offset_probes: yProbes,
  };
}

// Formatting and the small pure helpers, pinned once rather than per scenario.
function formattingVectors() {
  const dict = JSON.parse(readFileSync(join(repo, 'spec', 'witness_dictionary.json'), 'utf8'));
  return {
    humanize: Object.fromEntries(
      dict.failure_types.rust_variants
        .concat(['heartbeat', 'key_rotation', 'lifecycle', 'SomeFutureSignal'])
        .map((v) => [v, T.humanizeWords(v)])),
    hour_minute: [0, 600, 24000, BASE + 6 * H + 2400].map((t) => ({ t, s: T.fmtHM(t) })),
    bucket_range: [{ t: 24000, size: 600 }, { t: BASE, size: 600 }, { t: BASE + 23 * H + 3000, size: 600 }]
      .map((b) => ({ t: b.t, size: b.size, s: T.fmtBucketRange(b.t, b.size) })),
    day_label: [0, BASE, BASE + DAY, 1767225600].map((t) => ({ t, s: T.fmtDayLabel(t) })),
    duration: [600, 3600, 3 * 3600 + 20 * 60, 2 * DAY, DAY + 5 * H, 90]
      .map((s) => ({ s, out: T.fmtDuration(s) })),
    greek_width: ['', 'Sound', 'Tamper detected', 'Large object crossed boundary',
      'A very long label that should clamp at the maximum width for the column']
      .map((l) => ({ label: l, w: T.greekWidth(l) })),
    zone_indent: (() => {
      const zones = ['zone:a', 'zone:b', 'zone:c', 'zone:d', 'zone:e', 'zone:f'];
      return zones.concat(['zone:missing']).map((z) => ({ zone: z, indent: T.zoneIndent(zones, z) }));
    })(),
  };
}

const fixture = {
  $comment: 'GENERATED by viewer/tools/gen_timeline_parity.mjs — do not hand-edit. '
    + 'Cross-language parity for the timeline scrub model: viewer/timeline_core.js (JS, the '
    + 'reference) and ios/Shared/TimelineScrub.swift (Swift) must both reproduce every value '
    + 'here. Regenerate and commit whenever a folding or layout rule changes.',
  layout_height: LAYOUT_HEIGHT,
  grid_min_gap: 22,
  formatting: formattingVectors(),
  scenarios: SCENARIOS.map((s) => ({
    name: s.name,
    why: s.why,
    coverage_t0: s.coverageT0 === undefined ? null : s.coverageT0,
    coverage_t1: s.coverageT1 === undefined ? null : s.coverageT1,
    records: s.records,
    expected: expectedFor(s),
  })),
};

const text = JSON.stringify(fixture, null, 2) + '\n';

if (process.argv.includes('--check')) {
  let current = '';
  try { current = readFileSync(OUT, 'utf8'); } catch { /* missing */ }
  if (current !== text) {
    console.error('tests/fixtures/timeline/scrub_parity.json is out of date — '
      + 'run `node viewer/tools/gen_timeline_parity.mjs` and commit.');
    process.exit(1);
  }
  console.log('timeline parity fixture is up to date.');
} else {
  mkdirSync(dirname(OUT), { recursive: true });
  writeFileSync(OUT, text);
  console.log('wrote ' + OUT);
}
