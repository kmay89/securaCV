// canary-local/tests/mic.test.js — drift-lock for the 4.3C mic sim.
//
// Pins assets/mic-sim.js to the canary-display mic firmware the same way
// mode.test.js pins the Mode sim: by reading the firmware's own source (the
// committed Arduino mirror, regenerated from src/ by setup.sh) and comparing
// constants — AND by replaying the host test's own scenarios through the JS
// port and demanding the SAME verdicts. A change on either side that isn't
// mirrored breaks CI here.
//
//   presets / windows / wake / wire names  <- arduino/canary_display/mic_logic.h
//   detector behavior                      <- the scenarios in tests_host/
//                                              test_mic_logic.cpp, re-run in JS
//
// Sections: "honesty" (the sim carries exactly the firmware's numbers) and
// "parity" (the ported detector decides exactly what the C++ core decides).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..", "..");
const MIC_H = join(
  ROOT,
  "firmware/projects/canary-display/arduino/canary_display/mic_logic.h"
);

async function importSim() {
  return import("../assets/mic-sim.js");
}

const num = (re, src, label) => {
  const m = src.match(re);
  assert.ok(m, `${label}: ${re} not found in mic_logic.h`);
  return Number(m[1]);
};

// ── honesty ─────────────────────────────────────────────────────────────────

test("sensitivity presets equal the firmware SENS_* rows", async () => {
  const { SENS, SENS_ORDER, SENS_DEFAULT_INDEX } = await importSim();
  const h = readFileSync(MIC_H, "utf8");
  const rows = [
    ...h.matchAll(/SENS_(QUIET|STANDARD|NOISY)\s*=\s*\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}/g),
  ];
  assert.strictEqual(rows.length, 3, "three presets in the firmware");
  for (const r of rows) {
    const name = r[1].toLowerCase();
    assert.deepStrictEqual(
      SENS[name],
      {
        floor_min: Number(r[2]),
        on_pct: Number(r[3]),
        off_pct: Number(r[4]),
        floor_shift: Number(r[5]),
      },
      `preset ${name}`
    );
  }
  // The order table (index -> preset) and default index match the firmware.
  assert.strictEqual(SENS_ORDER.length, num(/SENS_COUNT = (\d+)/, h, "SENS_COUNT"));
  assert.strictEqual(
    SENS_DEFAULT_INDEX,
    num(/SENS_DEFAULT_INDEX = (\d+)/, h, "SENS_DEFAULT_INDEX")
  );
  // sensitivity_by_index: case 0 -> QUIET, case 2 -> NOISY, default STANDARD.
  assert.strictEqual(SENS_ORDER[0], "quiet");
  assert.strictEqual(SENS_ORDER[2], "noisy");
  assert.strictEqual(SENS_ORDER[SENS_DEFAULT_INDEX], "standard");
});

test("envelope + cadence + transient constants equal the firmware", async () => {
  const { FALL_SHIFT, CADENCE, TRANSIENT } = await importSim();
  const h = readFileSync(MIC_H, "utf8");
  assert.strictEqual(FALL_SHIFT, num(/FALL_SHIFT = (\d+)/, h, "FALL_SHIFT"));

  assert.strictEqual(CADENCE.T3_BEEP_MIN, num(/T3_BEEP_MIN = (\d+)/, h, "T3_MIN"));
  assert.strictEqual(CADENCE.T3_BEEP_MAX, num(/T3_BEEP_MAX = (\d+)/, h, "T3_MAX"));
  assert.strictEqual(CADENCE.T4_BEEP_MIN, num(/T4_BEEP_MIN = (\d+)/, h, "T4_MIN"));
  assert.strictEqual(CADENCE.T4_BEEP_MAX, num(/T4_BEEP_MAX = (\d+)/, h, "T4_MAX"));
  assert.strictEqual(CADENCE.GROUP_GAP_MS, num(/GROUP_GAP_MS = (\d+)/, h, "GROUP_GAP"));
  assert.strictEqual(CADENCE.RESET_GAP_MS, num(/RESET_GAP_MS = (\d+)/, h, "RESET_GAP"));
  // The windows must stay disjoint (the firmware's cross-classify guard).
  assert.ok(CADENCE.T3_BEEP_MIN > CADENCE.T4_BEEP_MAX, "T3/T4 windows disjoint");

  assert.strictEqual(TRANSIENT.RISE_PCT, num(/rise_pct = (\d+)/, h, "rise_pct"));
  assert.strictEqual(
    TRANSIENT.REFRACTORY_MS,
    num(/refractory_ms = (\d+)/, h, "refractory_ms")
  );
});

test("event wire names equal the firmware classifier vocabulary", async () => {
  const { eventWireName } = await importSim();
  const h = readFileSync(MIC_H, "utf8");
  assert.ok(h.includes('return "acoustic_smoke_alarm";'), "smoke wire name in fw");
  assert.ok(h.includes('return "acoustic_co_alarm";'), "co wire name in fw");
  assert.strictEqual(eventWireName("smoke_t3"), "acoustic_smoke_alarm");
  assert.strictEqual(eventWireName("co_t4"), "acoustic_co_alarm");
  assert.strictEqual(eventWireName("none"), "");
  // These carry the classifier's Alert substrings (smoke / co_alarm).
  assert.ok(eventWireName("smoke_t3").includes("smoke"));
  assert.ok(eventWireName("co_t4").includes("co_alarm"));
});

test("reassurance copy is present, honest, and fleet-clean", async () => {
  const { MIC_FACTS } = await importSim();
  assert.ok(MIC_FACTS.does.length >= 3 && MIC_FACTS.doesNot.length >= 3);
  const blob = JSON.stringify(MIC_FACTS).toLowerCase();
  assert.ok(!/flock/.test(blob), "it is a fleet, never a flock");
  assert.ok(/never records/.test(blob), "the core 'never records' claim is stated");
  assert.ok(/three beeps/.test(blob) && /four beeps/.test(blob),
    "the T3/T4 beep counts are named in the copy");
  assert.ok(/loudness number/.test(blob), "the scalar-not-samples barrier is stated");
});

test("no bird-group word anywhere in the sim", async () => {
  const src = readFileSync(join(__dirname, "..", "assets/mic-sim.js"), "utf8");
  assert.ok(!/flock/i.test(src), "the word is fleet");
});

// ── parity: replay tests_host/test_mic_logic.cpp through the JS port ─────────

// Port of play_group(): `beeps` pulses of `on_ms` (RMS 2000), separated by
// `gap_ms` of silence, then `rest_ms`. Frames every 25 ms. Returns the last
// non-none detection seen. Threads the clock through `clk` like the C++ `t`.
function playGroup(env, det, clk, beeps, on_ms, gap_ms, rest_ms) {
  let last = { event: "none", cycles: 0, confidence: 0 };
  const frames = (span, rms) => {
    for (let i = 0; i < span; i += 25) {
      const loud = env.update(rms);
      const d = det.update(loud, clk.t);
      if (d.event !== "none") last = d;
      clk.t += 25;
    }
  };
  for (let b = 0; b < beeps; b++) {
    frames(on_ms, 2000);
    frames(b + 1 < beeps ? gap_ms : 0, 0);
  }
  frames(rest_ms, 0);
  return last;
}

test("the gate: off by default, pins block arming, disarm is a hard stop",
  async () => {
    const { Gate, indicatorLit } = await importSim();
    const g = new Gate();
    assert.strictEqual(g.update(), "none", "fresh gate does nothing");
    assert.ok(!g.running && !indicatorLit(g), "fresh gate dark");

    g.armed = true; // armed, but pins unset
    assert.strictEqual(g.update(), "none", "armed with unset pins never starts");
    assert.ok(!g.running, "mics stay provably un-driven");
    g.pins_ok = true;
    assert.strictEqual(g.update(), "start", "pins filled at bench -> start");
    assert.ok(g.running && indicatorLit(g), "driver up == chip lit, same bit");
    g.armed = false;
    assert.strictEqual(g.update(), "stop", "disarm -> Stop (driver uninstalled)");
    assert.ok(!g.running && !indicatorLit(g), "stopped == chip dark");
    assert.strictEqual(g.update(), "none", "idempotent after stop");
  });

test("the indicator cannot desync: lit IS running across every state",
  async () => {
    const { Gate, indicatorLit } = await importSim();
    const g = new Gate();
    for (let armed = 0; armed <= 1; armed++) {
      for (let pins = 0; pins <= 1; pins++) {
        g.armed = !!armed;
        g.pins_ok = !!pins;
        g.update();
        assert.strictEqual(indicatorLit(g), g.running, "indicator == driver");
        assert.strictEqual(g.running, !!(armed && pins), "runs iff armed AND pins");
      }
    }
  });

test("T3 smoke needs two cycles; streak + confidence carried", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  const env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  let d = playGroup(env, det, clk, 3, 500, 500, 1500);
  assert.strictEqual(d.event, "none", "one T3 group is a horn, not an alarm");
  d = playGroup(env, det, clk, 3, 500, 500, 1500);
  assert.strictEqual(d.event, "smoke_t3", "second on-grammar cycle raises smoke");
  assert.ok(d.cycles === 2 && d.confidence >= 50, "cycles + confidence carried");
  d = playGroup(env, det, clk, 3, 500, 500, 1500);
  assert.ok(d.event === "smoke_t3" && d.cycles === 3, "streak keeps growing");
});

test("T4 CO detects on two cycles", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  const env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  playGroup(env, det, clk, 4, 100, 100, 5000);
  const d = playGroup(env, det, clk, 4, 100, 100, 5000);
  assert.strictEqual(d.event, "co_t4", "two T4 cycles raise CO");
});

test("doorbell and speech-shaped noise never alarm", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  const env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  let d = playGroup(env, det, clk, 2, 700, 300, 2000);
  assert.strictEqual(d.event, "none", "doorbell is not an alarm");
  d = playGroup(env, det, clk, 3, 1200, 400, 2000);
  assert.strictEqual(d.event, "none", "long slurred bursts rejected (duration)");
  d = playGroup(env, det, clk, 5, 150, 150, 2000);
  assert.strictEqual(d.event, "none", "five-beat patter rejected (count)");
});

test("silence resets a stale streak", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  const env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  playGroup(env, det, clk, 3, 500, 500, 1500); // streak 1, then stops
  let d = playGroup(env, det, clk, 0, 0, 0, 20000); // dead room > reset gap
  assert.strictEqual(d.event, "none", "silence raises nothing");
  d = playGroup(env, det, clk, 3, 500, 500, 1500);
  assert.strictEqual(d.event, "none", "a stale streak lends no credit after quiet");
});

test("switching alarm type needs fresh two cycles", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  const env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  playGroup(env, det, clk, 3, 500, 500, 1500); // SmokeT3 = 1
  let d = playGroup(env, det, clk, 4, 100, 100, 5000);
  assert.strictEqual(d.event, "none", "type switch resets the streak to one");
  d = playGroup(env, det, clk, 4, 100, 100, 5000);
  assert.strictEqual(d.event, "co_t4", "second matching T4 cycle raises CO");
});

test("count and duration windows are disjoint (no cross-classify)", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  let env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  // 3 SHORT beeps: right count for T3, wrong duration -> neither.
  playGroup(env, det, clk, 3, 100, 100, 1500);
  let d = playGroup(env, det, clk, 3, 100, 100, 1500);
  assert.strictEqual(d.event, "none", "3 short beeps are not T3");
  // 4 LONG beeps: right count for T4, wrong duration -> neither.
  env = new Envelope(); det = new CadenceDetector(); clk = { t: 0 };
  playGroup(env, det, clk, 4, 500, 500, 5000);
  d = playGroup(env, det, clk, 4, 500, 500, 5000);
  assert.strictEqual(d.event, "none", "4 long beeps are not T4");
});

test("collapsed timing fails safe: zero-duration beeps never alarm", async () => {
  const { CadenceDetector, CADENCE } = await importSim();
  const det = new CadenceDetector();
  for (let b = 0; b < 3; b++) {
    det.update(true, 1000); // rising at now == edge_ms
    det.update(false, 1000); // falling same instant -> dur 0
  }
  let d = { event: "none" };
  const closeAt = 1000 + CADENCE.GROUP_GAP_MS + 100;
  for (let q = 1000; q <= closeAt; q += 25) d = det.update(false, q);
  assert.strictEqual(d.event, "none", "zero-duration beeps never raise an alarm");
});

test("confidence grows and caps at 95", async () => {
  const { Envelope, CadenceDetector } = await importSim();
  const env = new Envelope(), det = new CadenceDetector(), clk = { t: 0 };
  playGroup(env, det, clk, 3, 500, 500, 1500); // cycle 1: no fire
  let lastConf = 0;
  for (let i = 0; i < 8; i++) {
    const d = playGroup(env, det, clk, 3, 500, 500, 1500);
    assert.strictEqual(d.event, "smoke_t3", "sustained T3 keeps firing");
    assert.ok(d.confidence >= lastConf, "confidence non-decreasing");
    assert.ok(d.confidence <= 95, "confidence caps at 95");
    lastConf = d.confidence;
  }
  assert.strictEqual(lastConf, 95, "a long alarm saturates confidence");
});

test("envelope hysteresis: on-threshold in, off-threshold out", async () => {
  const { Envelope, SENS } = await importSim();
  const env = new Envelope();
  env.setProfile(SENS.standard);
  assert.ok(!env.update(300), "quiet below on-threshold stays quiet");
  assert.ok(env.update(2000), "a loud beep crosses on-threshold");
  assert.ok(env.update(500), "decaying tail between thresholds holds loud");
  assert.ok(!env.update(300), "below off-threshold releases");
});

test("a beep does not inflate the floor it must clear", async () => {
  const { Envelope, SENS } = await importSim();
  const env = new Envelope();
  env.setProfile(SENS.standard);
  for (let i = 0; i < 40; i++) env.update(200); // settle on a quiet room
  const base = env.noiseFloor();
  for (let i = 0; i < 20; i++) env.update(2000); // a 500 ms beep
  for (let i = 0; i < 40; i++) env.update(200); // the gap after it
  assert.ok(env.noiseFloor() <= base + 40, "the beep's gap pulls the floor back");
});

test("the floor tracks ambient; the bar rises with the room", async () => {
  const { Envelope, SENS } = await importSim();
  const env = new Envelope();
  env.setProfile(SENS.standard);
  const onQuiet = env.onThreshold();
  for (let i = 0; i < 400; i++) env.update(800); // steady room hum
  assert.ok(env.noiseFloor() > 600, "floor climbs toward the 800 ambient");
  assert.ok(env.onThreshold() > onQuiet + 500, "the bar rises with the room");
});

test("self-calibration: the same level reads differently by room", async () => {
  const { Envelope, SENS } = await importSim();
  const loud = new Envelope();
  loud.setProfile(SENS.standard);
  for (let i = 0; i < 600; i++) loud.update(900); // a loud floor
  assert.ok(!loud.update(2000), "2000 is below the raised bar in a loud room");
  assert.ok(loud.update(4000), "a genuinely louder alarm still crosses");
  const quiet = new Envelope();
  quiet.setProfile(SENS.standard);
  assert.ok(quiet.update(2000), "the same 2000 IS a beep against a silent floor");
});

test("presets order by sensitivity (quiet < standard < noisy)", async () => {
  const { Envelope, SENS, sensByIndex, sensName, SENS_DEFAULT_INDEX } =
    await importSim();
  const onAt = (s, floor) => {
    const e = new Envelope();
    e.setProfile(s);
    e.floor = floor;
    return e.onThreshold();
  };
  const f = 500;
  assert.ok(onAt(SENS.quiet, f) < onAt(SENS.standard, f), "quiet more sensitive");
  assert.ok(onAt(SENS.standard, f) < onAt(SENS.noisy, f), "standard < noisy");
  assert.strictEqual(sensByIndex(0).on_pct, SENS.quiet.on_pct, "index 0 = quiet");
  assert.strictEqual(
    sensByIndex(SENS_DEFAULT_INDEX).on_pct,
    SENS.standard.on_pct,
    "default index is standard"
  );
  assert.strictEqual(sensName(2), "noisy", "index 2 names noisy");
});

test("wake-on-sound fires once on a loud onset; held-loud doesn't re-wake",
  async () => {
    const { TransientDetector } = await importSim();
    const t = new TransientDetector();
    const floor = 200;
    const thr = Math.floor((floor * t.rise_pct) / 100);
    let now = 0;
    for (let i = 0; i < 5; i++) {
      assert.ok(!t.update(50, floor, now), "quiet doesn't wake");
      now += 20;
    }
    assert.ok(t.update(thr + 2000, floor, now), "loud onset wakes"); now += 20;
    assert.ok(!t.update(thr + 2000, floor, now), "held-loud doesn't re-wake");
  });

test("wake-on-sound refractory debounces repeats", async () => {
  const { TransientDetector } = await importSim();
  const t = new TransientDetector();
  const floor = 200, hi = 4000;
  let now = 0;
  t.update(50, floor, now); now += 20; // seed
  assert.ok(t.update(hi, floor, now), "first onset wakes");
  now += 100; t.update(50, floor, now);
  now += 100; assert.ok(!t.update(hi, floor, now), "a second onset within 4 s suppressed");
  now += t.refractory_ms; t.update(50, floor, now);
  now += 20; assert.ok(t.update(hi, floor, now), "after the gap it wakes again");
});

test("wake-on-sound ignores the arming glitch (seeds on first quiet frame)",
  async () => {
    const { TransientDetector } = await importSim();
    const t = new TransientDetector();
    const floor = 200, hi = 4000;
    let now = 0;
    assert.ok(!t.update(hi, floor, now), "loud-at-start does not wake"); now += 20;
    assert.ok(!t.update(hi, floor, now), "still loud, still no wake"); now += 20;
    t.update(50, floor, now); now += 20; // room quiet -> seeded
    assert.ok(t.update(hi, floor, now), "now a real onset wakes");
  });

test("wake-on-sound scales with the room", async () => {
  const { TransientDetector } = await importSim();
  const quiet = new TransientDetector(), loud = new TransientDetector();
  let now = 0;
  quiet.update(50, 200, now); loud.update(50, 1500, now); now += 20;
  const mid = 2000; // +20 dB over 200, but below 1500*6
  assert.ok(quiet.update(mid, 200, now), "2000 is an onset over a quiet floor");
  assert.ok(!loud.update(mid, 1500, now), "the same 2000 is not over a loud floor");
});

test("MicSim end-to-end: opt-in wake, sensitivity is the one tunable axis",
  async () => {
    const { MicSim, SENS, SENS_DEFAULT_INDEX } = await importSim();
    const sim = new MicSim();
    assert.strictEqual(sim.sensIndex, SENS_DEFAULT_INDEX, "defaults to standard");
    // Wake is opt-in: off by default, no wake even on a loud onset.
    let now = 0, sawWake = false;
    for (let i = 0; i < 5; i++) { sim.feed(50, now); now += 20; }
    if (sim.feed(6000, now).wake) sawWake = true; now += 20;
    assert.ok(!sawWake, "wake stays silent until opted in");
    // Opt in, and the same shape wakes once.
    sim.reset();
    sim.wakeOnSound = true;
    now = 0;
    for (let i = 0; i < 5; i++) { sim.feed(50, now); now += 20; }
    assert.ok(sim.feed(6000, now).wake, "opted-in loud onset wakes the screen");
    // Sensitivity swaps the profile (quiet trips lower than noisy).
    sim.setSensitivity(0);
    assert.strictEqual(sim.env.s.on_pct, SENS.quiet.on_pct);
    sim.setSensitivity(2);
    assert.strictEqual(sim.env.s.on_pct, SENS.noisy.on_pct);
  });
