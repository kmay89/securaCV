// canary-local/tests/radar_dev.test.js — the Proving Ground's honesty gate.
//
// Three jobs:
//  1. Pin the twin's knob tables to the firmware's own bounds/defaults
//     (sense_config.h SENSE_*_LO/HI, configs/canary-sense/*/config.h CS_*),
//     so a firmware retune breaks this file, not a user's calibration.
//  2. Pin the EmuTuningConsole to the tuning-console grammar the firmware's
//     host tests pin (tests_host/test_tuning_console.cpp) — and prove every
//     reply round-trips through the REAL bench parsers (flash-core.js), so
//     the twin can never drift into a dialect the live bench wouldn't parse.
//  3. Drive the whole twin end-to-end (scene → wire bytes → parser → FSMs →
//     console lines → DrillEngine) and prove the drills settle the way the
//     page promises: the "tests it in one" property.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");

const read = (p) => readFileSync(p, "utf8");
const cfgHdr = read(join(REPO, "firmware/projects/canary-sense/include/canary/sense_config.h"));
const cfgDefault = read(join(REPO, "firmware/configs/canary-sense/default/config.h"));
const cfgWellbeing = read(join(REPO, "firmware/configs/canary-sense/wellbeing/config.h"));
const consoleHdr = read(join(REPO, "firmware/common/console/tuning_console.h"));
const mainCpp = read(join(REPO, "firmware/projects/canary-sense/src/main.cpp"));
const senselab = JSON.parse(read(join(ROOT, "devices/senselab.json")));
const buildLine = JSON.parse(read(join(ROOT, "build-line.json")));

// Matches both styles of firmware constant: `#define NAME 300` and
// `… constexpr … NAME = 96;`.
const cint = (src, macro) =>
  Number(src.match(new RegExp("[#\\w ]\\b" + macro + "\\b\\s*=?\\s*(\\d+)"))[1]);

async function mods() {
  const emu = await import("../assets/radar-emu.js");
  const drills = await import("../assets/radar-drills.js");
  const core = await import("../assets/flash-core.js");
  return { emu, drills, core };
}

// ── 1. knob tables are the firmware's own numbers ──────────────────────────

test("presence knob bounds/defaults mirror sense_config.h + default config.h", async () => {
  const { emu } = await mods();
  const want = {
    debounce: ["SENSE_DEBOUNCE_MS_LO", "SENSE_DEBOUNCE_MS_HI", "CS_PRESENT_DEBOUNCE_MS"],
    clear: ["SENSE_CLEAR_MS_LO", "SENSE_CLEAR_MS_HI", "CS_CLEAR_TIMEOUT_MS"],
    stall: ["SENSE_STALL_MS_LO", "SENSE_STALL_MS_HI", "CS_RADAR_STALL_MS"],
    near: ["SENSE_NEAR_CM_LO", "SENSE_NEAR_CM_HI", "CS_RANGE_NEAR_CM"],
    mid: ["SENSE_MID_CM_LO", "SENSE_MID_CM_HI", "CS_RANGE_MID_CM"],
  };
  assert.strictEqual(emu.PRESENCE_KNOBS.length, Object.keys(want).length);
  for (const k of emu.PRESENCE_KNOBS) {
    const [lo, hi, def] = want[k.name];
    assert.strictEqual(k.lo, cint(cfgHdr, lo), k.name + ".lo");
    assert.strictEqual(k.hi, cint(cfgHdr, hi), k.name + ".hi");
    assert.strictEqual(k.def, cint(cfgDefault, def), k.name + ".def");
  }
});

test("vitals knob bounds/defaults mirror sense_config.h + wellbeing config.h", async () => {
  const { emu } = await mods();
  const want = {
    vlock: ["SENSE_VLOCK_MS_LO", "SENSE_VLOCK_MS_HI", "CS_VITALS_LOCK_MS"],
    vlost: ["SENSE_VLOST_MS_LO", "SENSE_VLOST_MS_HI", "CS_VITALS_LOST_MS"],
    breath_min: ["SENSE_BREATH_MIN_LO", "SENSE_BREATH_MIN_HI", "CS_BREATH_MIN_BPM"],
    breath_max: ["SENSE_BREATH_MAX_LO", "SENSE_BREATH_MAX_HI", "CS_BREATH_MAX_BPM"],
    heart_min: ["SENSE_HEART_MIN_LO", "SENSE_HEART_MIN_HI", "CS_HEART_MIN_BPM"],
    heart_max: ["SENSE_HEART_MAX_LO", "SENSE_HEART_MAX_HI", "CS_HEART_MAX_BPM"],
  };
  assert.strictEqual(emu.VITALS_KNOBS.length, Object.keys(want).length);
  for (const k of emu.VITALS_KNOBS) {
    const [lo, hi, def] = want[k.name];
    assert.strictEqual(k.lo, cint(cfgHdr, lo), k.name + ".lo");
    assert.strictEqual(k.hi, cint(cfgHdr, hi), k.name + ".hi");
    assert.strictEqual(k.def, cint(cfgWellbeing, def), k.name + ".def");
  }
});

test("every knob token exists in the firmware's SENSE_KNOBS table", async () => {
  const { emu } = await mods();
  for (const k of [...emu.PRESENCE_KNOBS, ...emu.VITALS_KNOBS]) {
    assert.ok(mainCpp.includes(`{"${k.name}", "${k.unit}"`),
      `main.cpp SENSE_KNOBS is missing {"${k.name}", "${k.unit}", …}`);
  }
});

test("console constants mirror tuning_console.h", async () => {
  const { emu } = await mods();
  assert.strictEqual(emu.MAX_LINE, cint(consoleHdr, "MAX_LINE"));
  assert.strictEqual(emu.STREAM_DEFAULT_MS, cint(consoleHdr, "STREAM_DEFAULT_MS"));
  assert.strictEqual(emu.STREAM_MIN_MS, cint(consoleHdr, "STREAM_MIN_MS"));
  assert.strictEqual(emu.STREAM_MAX_MS, cint(consoleHdr, "STREAM_MAX_MS"));
});

// ── 2. the console grammar (test_tuning_console.cpp's cases, in JS) ────────

function bench(emu, wellbeing = false) {
  const values = {};
  for (const k of emu.knobTable(wellbeing)) values[k.name] = k.def;
  const lines = [];
  const con = new emu.EmuTuningConsole(
    emu.knobTable(wellbeing).map((k) => ({
      ...k,
      get: () => values[k.name],
      set: (v) => {
        const c = Math.min(Math.max(v, k.lo), k.hi);
        const changed = values[k.name] !== c;
        values[k.name] = c;
        return changed;
      },
    })),
    (l) => lines.push(l),
  );
  return { con, lines, values, drain: () => lines.splice(0) };
}

test("cfg snapshot: one line, every knob, stream + raw — parses with the real bench parser", async () => {
  const { emu, core } = await mods();
  const { con, drain } = bench(emu, true);
  con.feed("cfg\n");
  const out = drain();
  assert.strictEqual(out.length, 1);
  const parsed = core.parseCfgLine(out[0]);
  assert.ok(parsed, "parseCfgLine refused the twin's [cfg] line: " + out[0]);
  for (const k of emu.knobTable(true)) assert.strictEqual(parsed.values[k.name], k.def);
  assert.strictEqual(parsed.stream, 1000);
  assert.strictEqual(parsed.raw, false);
});

test("set clamps and says so; the verdict parses with the real parser", async () => {
  const { emu, core } = await mods();
  const { con, values, drain } = bench(emu);
  con.feed("set debounce 999999\n");
  const out = drain();
  const v = core.parseTuneLine(out[0]);
  assert.ok(v && v.ok, out[0]);
  assert.match(out[0], /debounce=3000 \(clamped from 999999; range 0\.\.3000\)/);
  assert.strictEqual(values.debounce, 3000);
  assert.ok(core.parseCfgLine(out[1]), "set must echo the refreshed [cfg]");
});

test("set to the same value answers ok (unchanged) — the no-op drill's contract", async () => {
  const { emu, core } = await mods();
  const { con, drain } = bench(emu);
  con.feed("set debounce 300\n");
  const out = drain();
  assert.match(out[0], /^\[tune\] ok debounce=300 \(unchanged\)$/);
  assert.ok(core.parseTuneLine(out[0]).ok);
});

test("unknown knob, non-number, and usage errors match the firmware's wording", async () => {
  const { emu } = await mods();
  const { con, drain } = bench(emu);
  con.feed("set nope 5\n");
  assert.match(drain()[0], /^\[tune\] err unknown knob 'nope' \(try 'help'\)$/);
  con.feed("set debounce -3\n");
  assert.match(drain()[0], /^\[tune\] err '-3' is not a whole number$/);
  con.feed("set debounce\n");
  assert.match(drain()[0], /^\[tune\] err usage: set <knob> <value>$/);
  con.feed("bogus\n");
  assert.match(drain()[0], /^\[tune\] err unknown command 'bogus' \(try 'help'\)$/);
});

test("reset restores defaults, and says 'already there' the second time", async () => {
  const { emu } = await mods();
  const { con, values, drain } = bench(emu);
  con.feed("set clear 5000\n");
  drain();
  con.feed("reset\n");
  let out = drain();
  assert.match(out[0], /^\[tune\] ok defaults restored$/);
  assert.strictEqual(values.clear, 1500);
  con.feed("reset\n");
  out = drain();
  assert.match(out[0], /^\[tune\] ok defaults restored \(already there\)$/);
});

test("stream on|off|<ms> with the firmware's clamps", async () => {
  const { emu } = await mods();
  const { con, drain } = bench(emu);
  con.feed("stream off\n");
  assert.match(drain()[0], /^\[tune\] ok stream off$/);
  assert.strictEqual(con.streamPeriodMs(), 0);
  con.feed("stream on\n");
  assert.match(drain()[0], /^\[tune\] ok stream every 1000 ms$/);
  con.feed("stream 50\n");
  assert.match(drain()[0], /^\[tune\] ok stream every 200 ms$/);
  con.feed("stream 99999\n");
  assert.match(drain()[0], /^\[tune\] ok stream every 10000 ms$/);
});

test("raw on|off, and its usage error", async () => {
  const { emu } = await mods();
  const { con, drain } = bench(emu);
  con.feed("raw on\n");
  assert.match(drain()[0], /^\[tune\] ok raw on$/);
  assert.strictEqual(con.rawEnabled(), true);
  con.feed("raw sideways\n");
  assert.match(drain()[0], /^\[tune\] err usage: raw on\|off$/);
});

test("case-insensitive commands, CRLF tolerated, blank lines ignored", async () => {
  const { emu, core } = await mods();
  const { con, drain } = bench(emu);
  con.feed("\n\r\n");
  assert.strictEqual(drain().length, 0);
  con.feed("CFG\r\n");
  assert.ok(core.parseCfgLine(drain()[0]));
  con.feed("SET DEBOUNCE 500\r\n");
  const out = drain();
  assert.match(out[0], /^\[tune\] ok debounce=500$/);
});

test("an overflowing line is discarded whole with the firmware's error", async () => {
  const { emu } = await mods();
  const { con, drain } = bench(emu);
  con.feed("set debounce " + "9".repeat(200) + "\n");
  const out = drain();
  assert.strictEqual(out.length, 1);
  assert.match(out[0], new RegExp(`^\\[tune\\] err line too long \\(max ${emu.MAX_LINE - 1} chars\\)$`));
});

test("help lists every command and every knob", async () => {
  const { emu } = await mods();
  const { con, drain } = bench(emu, true);
  con.feed("?\n");
  const out = drain().join("\n");
  for (const cmd of ["help", "cfg", "set <knob> <value>", "reset", "stream on|off|<ms>", "raw on|off"])
    assert.ok(out.includes(cmd), "help is missing: " + cmd);
  for (const k of emu.knobTable(true))
    assert.ok(out.includes(k.name) && out.includes(k.help), "help is missing knob: " + k.name);
});

// ── 3. the twin end-to-end ─────────────────────────────────────────────────

const HW = senselab.hardware;

function makeTwin(emu, wellbeing) {
  const twin = new emu.EmuSense({ wellbeing, hw: HW, seed: 0x5e42 });
  const lines = [];
  twin.onLine = (l) => lines.push(l);
  return { twin, lines, drain: () => lines.splice(0) };
}

test("twin: a person walking in flips presence within the debounce, and out again", async () => {
  const { emu, core } = await mods();
  const { twin, drain } = makeTwin(emu, false);
  let now = 0;
  const step = (ms) => { for (let t = 0; t < ms; t += emu.TICK_MS) twin.tick((now += emu.TICK_MS)); };

  step(1000); // empty room settles to clear
  let out = drain();
  assert.ok(out.some((l) => { const e = core.parseSenseLine(l); return e && e.presence === "clear"; }),
    "an empty room must report clear: " + out.join(" | "));

  twin.setScene({ person: { x: 2.5, moving: true } });
  step(1000);
  out = drain();
  assert.ok(out.some((l) => /^\[presence\] -> present$/.test(l)),
    "walking in must print the transition line: " + out.join(" | "));
  assert.ok(out.some((l) => { const e = core.parseSenseLine(l); return e && e.kind === "sense" && e.presence === "present"; }));

  twin.setScene({ person: { x: 7.5, moving: false } });
  step(3000); // clear timeout is 1500 ms
  out = drain();
  assert.ok(out.some((l) => /^\[presence\] -> clear$/.test(l)),
    "leaving must clear: " + out.join(" | "));
});

test("twin: the [radar] stream line carries the coarse triple + errs and parses as kind radar", async () => {
  const { emu, core } = await mods();
  const { twin, drain } = makeTwin(emu, false);
  let now = 0;
  for (let i = 0; i < 25; i++) twin.tick((now += emu.TICK_MS));
  const radar = drain().map(core.parseSenseLine).filter((e) => e && e.kind === "radar");
  assert.ok(radar.length >= 2, "the stream must tick at 1000 ms");
  assert.ok(["present", "clear", "unknown"].includes(radar[0].presence));
  assert.ok(Number.isFinite(radar[0].frame_errs));
});

test("twin: raw on appends bench scalars the real parser reads back", async () => {
  const { emu, core } = await mods();
  const { twin, drain } = makeTwin(emu, false);
  twin.setScene({ person: { x: 1.2, moving: true } });
  twin.write("raw on\n");
  let now = 0;
  for (let i = 0; i < 25; i++) twin.tick((now += emu.TICK_MS));
  const withRaw = drain().map(core.parseSenseLine).filter((e) => e && e.kind === "radar" && e.raw);
  assert.ok(withRaw.length >= 1, "raw on must surface raw_dist in the stream");
  assert.ok(withRaw.some((e) => e.raw.dist_cm > 60 && e.raw.dist_cm < 220),
    "raw distance should sit near the staged 1.2 m");
});

test("twin: set over the console reconfigures the live FSM (a long clear holds presence)", async () => {
  const { emu, core } = await mods();
  const { twin, drain } = makeTwin(emu, false);
  let now = 0;
  const step = (ms) => { for (let t = 0; t < ms; t += emu.TICK_MS) twin.tick((now += emu.TICK_MS)); };
  twin.setScene({ person: { x: 2.0, moving: true } });
  step(1000);
  twin.write("set clear 8000\n");
  const echo = drain();
  assert.ok(echo.some((l) => core.parseTuneLine(l) && core.parseTuneLine(l).ok));
  assert.ok(echo.some((l) => /^\[CFG\] Radar reflexes:/.test(l)), "the [CFG] applied echo must print");
  twin.setScene({ person: { x: 7.5 } });
  step(4000); // was clear=1500; now 8000 — must still be present
  const out = drain();
  assert.ok(!out.some((l) => /^\[presence\] -> clear$/.test(l)),
    "presence must hold through the raised clear timeout");
  step(6000);
  assert.ok(drain().some((l) => /^\[presence\] -> clear$/.test(l)),
    "…and still clear once the new timeout elapses");
});

test("twin: unplugging the radar stalls to unknown with the firmware's line", async () => {
  const { emu } = await mods();
  const { twin, drain } = makeTwin(emu, false);
  let now = 0;
  const step = (ms) => { for (let t = 0; t < ms; t += emu.TICK_MS) twin.tick((now += emu.TICK_MS)); };
  twin.setScene({ person: { x: 2.0, moving: true } });
  step(1000);
  drain();
  twin.setScene({ unplugged: true });
  step(6000); // stall timeout is 5000 ms
  assert.ok(drain().some((l) => /^\[presence\] -> unknown \(radar stall\)$/.test(l)));
});

test("twin (wellbeing): a settled single person locks vitals and prints live BPM", async () => {
  const { emu, core } = await mods();
  const { twin, drain } = makeTwin(emu, true);
  let now = 0;
  const step = (ms) => { for (let t = 0; t < ms; t += emu.TICK_MS) twin.tick((now += emu.TICK_MS)); };
  twin.setScene({ person: { x: 0.9, moving: false, posture: "sitting", orientation: "facing" } });
  step(20000);
  const out = drain();
  assert.ok(out.some((l) => /^\[vitals\] breathing locked$/.test(l)), "the lock line must print");
  const bpm = out.map(core.parseSenseLine).filter((e) => e && e.kind === "bpm");
  assert.ok(bpm.length >= 1, "live BPM lines must print once locked");
  assert.ok(bpm.some((e) => e.breath >= 6 && e.breath <= 30 && e.heart >= 40 && e.heart <= 130));
});

test("twin (wellbeing): a second person suppresses the vitals lock", async () => {
  const { emu } = await mods();
  const { twin, drain } = makeTwin(emu, true);
  let now = 0;
  const step = (ms) => { for (let t = 0; t < ms; t += emu.TICK_MS) twin.tick((now += emu.TICK_MS)); };
  twin.setScene({ person: { x: 0.9, moving: false, posture: "sitting" }, secondPerson: true });
  step(20000);
  assert.ok(!drain().some((l) => /^\[vitals\] breathing locked$/.test(l)),
    "count 2+ must hard-suppress the lock");
});

// ── 4. the drill suite against the twin — "tests it in one" ────────────────

async function twinWithEngine(wellbeing) {
  const { emu, drills, core } = await mods();
  const { twin } = makeTwin(emu, wellbeing);
  const engine = new drills.DrillEngine(
    drills.drillsFor(wellbeing), (cmd) => twin.write(cmd + "\n"), wellbeing);
  let now = 0;
  twin.onLine = (l) => {
    const ev = core.parseCfgLine(l) || core.parseTuneLine(l) || core.parseSenseLine(l);
    if (ev) engine.feed(ev, now);
  };
  const step = (ms) => {
    for (let t = 0; t < ms; t += emu.TICK_MS) { twin.tick((now += emu.TICK_MS)); engine.tick(now); }
  };
  return { twin, engine, step, nowRef: () => now };
}

test("auto drills all pass against the twin", async () => {
  const { twin, engine, step, nowRef } = await twinWithEngine(false);
  step(500);
  for (const id of ["console", "stream", "roundtrip", "clean"]) {
    engine.arm(id, nowRef());
    step(9000);
    assert.ok(engine.results[id], id + " never settled");
    assert.strictEqual(engine.results[id].status, "pass",
      id + ": " + (engine.results[id].reason || ""));
  }
  void twin;
});

test("guided drills pass when the scene does what the page asks", async () => {
  const { twin, engine, step, nowRef } = await twinWithEngine(false);
  step(500);

  engine.arm("walk_in", nowRef());
  twin.setScene({ person: { x: 2.5, moving: true } });
  step(2000);
  assert.strictEqual(engine.results.walk_in.status, "pass");

  engine.arm("bands", nowRef());
  for (const x of [1.0, 2.5, 4.5]) { twin.setScene({ person: { x } }); step(1500); }
  assert.strictEqual(engine.results.bands.status, "pass", JSON.stringify(engine.results.bands));

  engine.arm("two_up", nowRef());
  twin.setScene({ secondPerson: true });
  step(1500);
  assert.strictEqual(engine.results.two_up.status, "pass");
  twin.setScene({ secondPerson: false });

  engine.arm("still_hold", nowRef());
  twin.setScene({ person: { x: 1.5, moving: false } });
  step(12000);
  assert.strictEqual(engine.results.still_hold.status, "pass", JSON.stringify(engine.results.still_hold));

  engine.arm("walk_out", nowRef());
  twin.setScene({ person: { x: 7.5 } });
  step(4000);
  assert.strictEqual(engine.results.walk_out.status, "pass");
});

test("a drill that the room ignores fails honestly by timeout", async () => {
  const { engine, step, nowRef } = await twinWithEngine(false);
  step(500);
  engine.arm("walk_in", nowRef());
  step(31000); // nobody walks in
  assert.strictEqual(engine.results.walk_in.status, "fail");
  assert.match(engine.results.walk_in.reason, /timed out/);
});

test("the wellbeing-only drill appears only on the wellbeing build", async () => {
  const { drills } = await mods();
  assert.ok(!drills.drillsFor(false).some((d) => d.id === "vitals_lock"));
  assert.ok(drills.drillsFor(true).some((d) => d.id === "vitals_lock"));
});

// ── 5. calibration planning ────────────────────────────────────────────────

test("calibPlan: medians, 10 cm rounding, console clamps, nested bands", async () => {
  const { drills } = await mods();
  const plan = drills.calibPlan([148, 152, 149, 151, 150], [352, 348, 350, 351, 349]);
  assert.strictEqual(plan.near_cm, 150);
  assert.strictEqual(plan.mid_cm, 350);
  assert.strictEqual(plan.notes.length, 0);

  const clamped = drills.calibPlan([20, 20, 20], [900, 900, 900]);
  assert.strictEqual(clamped.near_cm, 50);  // SENSE_NEAR_CM_LO
  assert.strictEqual(clamped.mid_cm, 600);  // SENSE_MID_CM_HI
  assert.ok(clamped.notes.length >= 2);

  const inverted = drills.calibPlan([300, 300, 300], [250, 250, 250]);
  assert.ok(inverted.mid_cm > inverted.near_cm, "bands must nest");

  assert.strictEqual(drills.calibPlan([], [100]).near_cm, null);
});

// ── 6. placement advice runs the real physics ──────────────────────────────

test("placementAdvice: a squared 2 m wall spot grades A; outside the sector grades D", async () => {
  const { emu, drills } = await mods();
  const scene = emu.defaultScene();
  scene.person.x = 2.0;
  const good = drills.placementAdvice(scene, HW, "presence");
  assert.strictEqual(good.grade, "A");

  scene.person.x = 0.8;
  scene.person.y = 3.5; // far off to the side — outside the 80° sector
  const out = drills.placementAdvice(scene, HW, "presence");
  assert.strictEqual(out.grade, "D");
  assert.ok(out.fixes.length >= 1);
});

test("placementAdvice: the bedside geometry scores well; across the room does not", async () => {
  const { emu, drills } = await mods();
  // Seeed's official install: ~1 m above the bed head, tilted 45° down at
  // the torso. A flat wall mount genuinely can't see a lying chest — the
  // scorer must reward the spec'd geometry and fail the flat one.
  const scene = emu.defaultScene();
  scene.mountHeight = 1.5;
  scene.tiltDeg = 45;
  scene.person = { x: 0.8, y: 0, posture: "lying", orientation: "facing", moving: false };
  const bed = drills.placementAdvice(scene, HW, "vitals");
  assert.ok(["A", "B"].includes(bed.grade), bed.headline);

  scene.tiltDeg = 0;
  const flat = drills.placementAdvice(scene, HW, "vitals");
  assert.strictEqual(flat.grade, "D", "a flat mount over a lying person must fail");

  scene.tiltDeg = 45;
  scene.person.x = 4.0;
  const far = drills.placementAdvice(scene, HW, "vitals");
  assert.strictEqual(far.grade, "D");
  assert.ok(far.fixes.length >= 1);
});

// ── 7. the page is wired into the Lab ──────────────────────────────────────

test("radar-dev.html is registered as a depth of the radar track in build-line.json", () => {
  const senseStage = buildLine.stages.find((s) => s.id === "sense");
  const radarTrack = senseStage.tracks.find((t) => t.track === "radar");
  const depths = radarTrack.benches[0].depths || [];
  assert.ok(depths.some((d) => d.lab === "radar-dev.html" && d.real === true),
    "radar-dev.html must be a real depth under The Sense");
});

test("the page shell loads its module and styles, and siblings link back", () => {
  const html = read(join(ROOT, "radar-dev.html"));
  for (const ref of ["assets/canary-local.css", "assets/flash.css", "assets/radar-dev.css", "assets/radar-dev.js"])
    assert.ok(html.includes(ref), "radar-dev.html is missing " + ref);
  assert.ok(read(join(ROOT, "sense.html")).includes("radar-dev.html"));
  assert.ok(read(join(ROOT, "senselab.html")).includes("radar-dev.html"));
  assert.ok(read(join(ROOT, "assets/flash.js")).includes("radar-dev.html"),
    "the Nursery's radar bench should point at the Proving Ground");
});
