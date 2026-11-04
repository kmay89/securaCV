// canary-local/tests/fixtures/mr60_presence_native.js — the firmware's
// PresenceFSM, built natively, for the Lab's two Sense ports to be held to.
//
// sense.test.js (assets/sense-ui.js's radar lab) and senselab.test.js
// (assets/sense-sim.js's Sense Lab) each port mr60_presence.cpp to JS. This
// builds the real file with the host's g++ (or $CXX) beside the small trace
// driver below, so a test can play one trace of frames through the firmware
// and through its port and compare every tick (sweep F131: the occupancy
// count follows the debounced state, and both ports must too).
//
// The driver also records what canary-sense main.cpp's drive_fsms would
// sign on each tick, by the rule that file states: presence_detected on a
// tick that turns Present, presence_cleared on one that turns Clear,
// occupancy_changed on a count change while Present. MAIN_GATES are those
// lines verbatim; a test asserts main.cpp still says them, so the driver
// cannot keep a rule the firmware dropped.
//
// The wellbeing build's vitals path rides the same driver (A107):
// mr60_vitals.cpp is built with -DCANARY_SENSE_VITALS beside the presence
// FSM, and when the caller passes a VitalsConfig the driver ticks it on
// every frame through drive_fsms' own vitals lines, VITALS_GATES, which are
// compiled into the driver as main.cpp spells them (a test asserts main.cpp
// still says them): the gate on the debounced count, and the snapshot
// update the state row reads. Each tick then also prints the lock, its
// change and stall flags, and the snapshot's breathing_locked, bpm_valid
// and both BPMs. mr60_presence.{h,cpp} carry no vitals branch (build()
// refuses if they grow one), so the presence rows of this build are the
// presence-only build's too.
//
// Without a compiler the build reports why (`skip`), and the caller skips
// with that reason, except under CI (the CI variable set, as GitHub Actions
// sets it), where a missing compiler is a failure: the runner has g++.

"use strict";

const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const { join } = require("node:path");

const REPO = join(__dirname, "..", "..", "..");
const MR60 = join(REPO, "firmware/common/sensors/mmwave_mr60");

const MAIN_GATES = [
  "    if (pev.state == Presence::Present) {\n      set_last_event(\"presence_detected\");\n"
    + "      record_event_now(\"presence_detected\", now);\n"
    + "    } else if (pev.state == Presence::Clear) {\n      set_last_event(\"presence_cleared\");\n"
    + "      record_event_now(\"presence_cleared\", now);\n    }",
  "  if (pev.count_changed) {\n    // Occupancy bucket moved (0/1/2+). Only newsworthy while someone is here.\n"
    + "    if (g_presence.state() == Presence::Present) {\n      set_last_event(\"occupancy_changed\");\n"
    + "      record_event_now(\"occupancy_changed\", now);",
];

// drive_fsms' vitals lines, verbatim: the lock is ticked on every frame
// with the debounced count's single-target verdict, and the state row's
// vitals fields move when the lock, bpm_valid or a valid BPM does. The
// driver compiles these strings as they stand, so the rows it prints are
// these lines' own.
const VITALS_GATES = [
  "  // Vitals are suppressed unless exactly one target is present.\n"
    + "  const bool single_target = (pev.count == CountBucket::One);\n"
    + "  const auto vev = g_vitals.tick(frame, single_target, now);\n\n"
    + "  const bool locked = (g_vitals.lock() == VitalsLock::Locked);",
  "  if (g_snap.breathing_locked != locked ||\n      g_snap.bpm_valid != vev.bpm_valid ||\n"
    + "      (vev.bpm_valid && (g_snap.breath_bpm != vev.breath_bpm ||\n"
    + "                         g_snap.heart_bpm != vev.heart_bpm))) {\n"
    + "    g_snap.breathing_locked = locked;\n    g_snap.bpm_valid  = vev.bpm_valid;\n"
    + "    g_snap.breath_bpm = vev.breath_bpm;\n    g_snap.heart_bpm  = vev.heart_bpm;",
];

// argv: present_debounce_ms clear_timeout_ms stall_timeout_ms near_cm mid_cm,
// then, for the wellbeing build, lock_confirm_ms lock_lost_ms breath_min_bpm
// breath_max_bpm heart_min_bpm heart_max_bpm.
// stdin, one tick per line: "R <t>" (reset), "N <t>" (no frame this loop),
// "P <t> <has_target 0|1> <target_count> <distance_cm>" (a presence frame),
// "V <t> <breath_bpm> <heart_bpm>" (a vitals frame).
// stdout, one line per tick: "<state> <count> <range> <state_changed>
// <count_changed> <stalled> <signed events, comma-joined, or ->", and with
// vitals " <lock> <lock_changed> <lock stalled> <breathing_locked>
// <bpm_valid> <breath_bpm> <heart_bpm>" (the last four the snapshot's).
const DRIVER = String.raw`
#include "mr60_presence.h"
#include "mr60_vitals.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace securacv::mmwave;
static const char* st(Presence p) {
  return p == Presence::Present ? "present" : p == Presence::Clear ? "clear" : "unknown";
}
static const char* ct(CountBucket c) {
  return c == CountBucket::One ? "1" : c == CountBucket::TwoPlus ? "2+" : "0";
}
static const char* rg(RangeBand r) {
  return r == RangeBand::Near ? "near" : r == RangeBand::Mid ? "mid" : r == RangeBand::Far ? "far" : "unknown";
}
static const char* lk(VitalsLock l) {
  return l == VitalsLock::Locked ? "locked" : l == VitalsLock::Lost ? "lost" : "unknown";
}
// the vitals fields of canary-sense's SenseSnapshot (include/canary/types.h)
struct Snap {
  bool     breathing_locked = false;
  bool     bpm_valid        = false;
  uint16_t breath_bpm       = 0;
  uint16_t heart_bpm        = 0;
};
int main(int argc, char** argv) {
  if (argc != 6 && argc != 12) return 2;
  const bool vitals = argc == 12;
  PresenceConfig cfg;
  cfg.present_debounce_ms = (uint32_t)std::strtoul(argv[1], nullptr, 10);
  cfg.clear_timeout_ms    = (uint32_t)std::strtoul(argv[2], nullptr, 10);
  cfg.stall_timeout_ms    = (uint32_t)std::strtoul(argv[3], nullptr, 10);
  cfg.near_cm             = (uint16_t)std::strtoul(argv[4], nullptr, 10);
  cfg.mid_cm              = (uint16_t)std::strtoul(argv[5], nullptr, 10);
  VitalsConfig vcfg;
  if (vitals) {
    vcfg.lock_confirm_ms = (uint32_t)std::strtoul(argv[6], nullptr, 10);
    vcfg.lock_lost_ms    = (uint32_t)std::strtoul(argv[7], nullptr, 10);
    vcfg.breath_min_bpm  = (uint16_t)std::strtoul(argv[8], nullptr, 10);
    vcfg.breath_max_bpm  = (uint16_t)std::strtoul(argv[9], nullptr, 10);
    vcfg.heart_min_bpm   = (uint16_t)std::strtoul(argv[10], nullptr, 10);
    vcfg.heart_max_bpm   = (uint16_t)std::strtoul(argv[11], nullptr, 10);
  }
  PresenceFSM fsm(cfg);
  VitalsFSM g_vitals(vcfg);
  Snap g_snap;
  char op;
  unsigned long t;
  while (std::scanf(" %c %lu", &op, &t) == 2) {
    if (op == 'R') { fsm.reset((uint32_t)t); g_vitals.reset((uint32_t)t); std::printf("reset\n"); continue; }
    Frame f;
    if (op == 'P') {
      unsigned has = 0, n = 0, cm = 0;
      if (std::scanf("%u %u %u", &has, &n, &cm) != 3) return 3;
      f.kind = FrameKind::Presence;
      f.has_target = has != 0;
      f.target_count = (uint8_t)n;
      f.distance_cm = (uint16_t)cm;
    } else if (op == 'V') {
      unsigned breath = 0, heart = 0;
      if (std::scanf("%u %u", &breath, &heart) != 2) return 3;
      f.kind = FrameKind::Vitals;
      f.breath_rate = (uint16_t)breath;
      f.heart_rate = (uint16_t)heart;
    } else if (op != 'N') {
      return 4;
    }
    const Frame& frame = f;
    const uint32_t now = (uint32_t)t;
    const PresenceEvent pev = fsm.tick(frame, now);
    std::string signed_events;
    if (pev.state_changed) {
      if (pev.state == Presence::Present) signed_events += "presence_detected,";
      else if (pev.state == Presence::Clear) signed_events += "presence_cleared,";
    }
    if (pev.count_changed && fsm.state() == Presence::Present) signed_events += "occupancy_changed,";
    if (signed_events.empty()) signed_events = "-";
    else signed_events.pop_back();
    std::printf("%s %s %s %d %d %d %s", st(fsm.state()), ct(fsm.count()), rg(fsm.range()),
                pev.state_changed ? 1 : 0, pev.count_changed ? 1 : 0, pev.stalled ? 1 : 0,
                signed_events.c_str());
    if (vitals) {
${VITALS_GATES[0]}
${VITALS_GATES[1]}
      }
      std::printf(" %s %d %d %d %d %u %u", lk(g_vitals.lock()), vev.lock_changed ? 1 : 0, vev.stalled ? 1 : 0,
                  g_snap.breathing_locked ? 1 : 0, g_snap.bpm_valid ? 1 : 0,
                  (unsigned)g_snap.breath_bpm, (unsigned)g_snap.heart_bpm);
    }
    std::printf("\n");
  }
  return 0;
}
`;

let built = null;

// Build once per process; { bin } or { skip: reason }. A build that fails
// with a compiler present throws: that is the firmware not compiling.
function build() {
  if (built) return built;
  const cxx = process.env.CXX || "g++";
  const probe = spawnSync(cxx, ["--version"], { encoding: "utf8" });
  if (probe.error || probe.status !== 0) {
    const why = `no C++ compiler (${cxx}; set CXX) to build mr60_presence.cpp and mr60_vitals.cpp`;
    if (process.env.CI) throw new Error(`${why}, and CI has one: refusing to skip`);
    built = { skip: why };
    return built;
  }
  // one build serves both flavors only while the presence FSM has no
  // vitals branch: refuse rather than hand a presence-only test the
  // wellbeing build's rows
  for (const f of ["mr60_presence.h", "mr60_presence.cpp"]) {
    if (fs.readFileSync(join(MR60, f), "utf8").includes("CANARY_SENSE_VITALS")) {
      throw new Error(`${f} now branches on CANARY_SENSE_VITALS: build the presence-only driver apart`);
    }
  }
  const dir = fs.mkdtempSync(join(os.tmpdir(), "securacv-mr60-presence-"));
  process.once("exit", () => fs.rmSync(dir, { recursive: true, force: true }));
  fs.writeFileSync(join(dir, "driver.cpp"), DRIVER);
  const bin = join(dir, "mr60_presence_driver");
  const args = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-DCANARY_SENSE_VITALS", "-I", MR60,
    join(dir, "driver.cpp"), join(MR60, "mr60_presence.cpp"), join(MR60, "mr60_vitals.cpp"), "-o", bin];
  const r = spawnSync(cxx, args, { encoding: "utf8" });
  if (r.status !== 0) throw new Error(`${cxx} could not build mr60_presence.cpp and mr60_vitals.cpp:\n${cxx} ${args.join(" ")}\n${r.stderr}`);
  built = { bin };
  return built;
}

// A trace is a list of ticks: { reset: t }, { t } (no frame),
// { t, target, count, cm } (a presence frame: has_target, target_count,
// distance_cm) or { t, breath, heart } (a vitals frame: breath_rate,
// heart_rate). cfg is the firmware's PresenceConfig by its field names;
// vcfg, when given, its VitalsConfig, and the run is the wellbeing build's.
// Returns one row per tick that is not a reset, each with a `vitals` record
// when vcfg is given.
function runFirmware(cfg, trace, vcfg) {
  const b = build();
  if (b.skip) throw new Error(b.skip);
  const lines = trace.map((k) => ("reset" in k ? `R ${k.reset}`
    : "target" in k ? `P ${k.t} ${k.target ? 1 : 0} ${k.count} ${k.cm}`
      : "breath" in k ? `V ${k.t} ${k.breath} ${k.heart}` : `N ${k.t}`));
  const argv = [cfg.present_debounce_ms, cfg.clear_timeout_ms, cfg.stall_timeout_ms, cfg.near_cm, cfg.mid_cm];
  if (vcfg) argv.push(vcfg.lock_confirm_ms, vcfg.lock_lost_ms, vcfg.breath_min_bpm, vcfg.breath_max_bpm,
    vcfg.heart_min_bpm, vcfg.heart_max_bpm);
  const r = spawnSync(b.bin, argv.map(String), { input: lines.join("\n") + "\n", encoding: "utf8", maxBuffer: 64 << 20 });
  if (r.status !== 0) throw new Error(`the mr60_presence driver exited ${r.status}: ${r.stderr}`);
  return r.stdout.trim().split("\n").filter((l) => l !== "reset").map((l) => {
    const [state, count, range, sc, cc, stalled, events, lock, lc, ls, bl, bv, breath, heart] = l.split(" ");
    const row = { state, count, range, state_changed: sc === "1", count_changed: cc === "1",
      stalled: stalled === "1", events: events === "-" ? [] : events.split(",") };
    if (vcfg) {
      row.vitals = { lock, lock_changed: lc === "1", stalled: ls === "1", breathing_locked: bl === "1",
        bpm_valid: bv === "1", breath_bpm: Number(breath), heart_bpm: Number(heart) };
    }
    return row;
  });
}

// The traces both ports are held to: each walks a room the way the decoder
// reports it (a presence flag frame whose count is still 0 before the
// radar's count frame lands, as after every no-target frame).
function traces(cfg) {
  const D = cfg.present_debounce_ms, C = cfg.clear_timeout_ms, S = cfg.stall_timeout_ms;
  const on = (t, count = 1, cm = 100) => ({ t, target: true, count, cm });
  const off = (t) => ({ t, target: false, count: 0, cm: 0 });
  const enter = (t0, n = 1) => [on(t0, 0), on(t0 + 50, n), on(t0 + 100, n, 200), on(t0 + D, n, 200)];
  const every = (from, to, step, mk) => {
    const out = [];
    for (let t = from; t < to; t += step) out.push(mk(t));
    return out;
  };
  return {
    // boot to Clear, a visit, the room empties
    emptying: [{ reset: 0 }, off(10), ...enter(1000), ...every(1000 + D + 100, 1000 + D + 100 + C, 100, off),
      off(1000 + D + 100 + C), off(1000 + D + 200 + C)],
    // one frame of no target, back with the count still 0, then the count frame
    dropout: [{ reset: 0 }, off(10), ...enter(1000), off(1500), on(1600, 0), on(1650, 1), on(1700, 1, 400)],
    // a debounce never earned, then two people, then one leaves
    debounce: [{ reset: 0 }, off(10), on(500, 0), on(550, 1), on(500 + D - 100, 1), off(500 + D - 50),
      ...enter(2000, 2), on(2000 + D + 100, 1), on(2000 + D + 200, 2)],
    // a target leaves and the radar goes silent inside the clear timeout
    stall: [{ reset: 0 }, off(10), ...enter(1000), off(1500), { t: 2000 }, { t: 1500 + S - 1 }, { t: 1500 + S },
      { t: 1500 + S + 10 }, on(1500 + S + 20, 1), on(1500 + S + 20 + D, 1), off(1500 + S + 40 + D)],
    // two visits: the second returns to a bucket the first visit had moved to
    revisit: [{ reset: 0 }, off(10), ...enter(1000), on(1500, 2), ...every(1600, 1600 + C, 100, off),
      off(1600 + C), ...enter(4000), on(4500, 2), on(4600, 1)],
    // a count beside a cleared flag: the radar's TARGET_COUNT frame lands
    // after PEOPLE_EXIST(0), so the decoder emits the count with has_target
    // false; it moves no bucket, and no target returns before the clear
    stray: [{ reset: 0 }, off(10), ...enter(1000), off(1500), { t: 1550, target: false, count: 2, cm: 0 },
      ...every(1600, 1500 + C, 100, off), off(1500 + C), off(1600 + C)],
    // a radar slow to speak at boot finds the room empty: its first frame
    // turns Unknown to Clear, and drive_fsms signs presence_cleared (A106)
    boot: [{ reset: 0 }, { t: 5 }, { t: S + 10 }, off(S + 20), off(S + 120), ...enter(S + 1000)],
    // the radar stalls with someone in the room and comes back to it empty,
    // then stalls over the empty room and comes back again: each return
    // turns Unknown to Clear and signs presence_cleared, the second with no
    // presence_detected between (A106)
    restart: [{ reset: 0 }, off(10), ...enter(1000), { t: 1000 + D + S }, off(1000 + D + S + 50),
      off(1000 + D + S + 150), { t: 1000 + D + 2 * S + 150 }, off(1000 + D + 2 * S + 200),
      ...enter(1000 + D + 2 * S + 1000)],
    // a seeded random walk of the wire (A106's review): visits, one-frame
    // dropouts, count-pending flags, strays, silences on every window's edge
    // and Unknown restarts, 10,000 ticks
    fuzz: fuzz(cfg, 0x5e115e, 10000),
  };
}

// mulberry32: a small seeded generator, so a failing fuzz trace replays.
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// A seeded trace of n ticks: a room whose head count wanders 0 to 3, read
// at about the radar's cadence, with the edges the FSM decides on hit
// exactly (a gap of each window, one less and one more), silences past the
// stall timeout, and a reset now and then (an Unknown restart, as
// reconfigure() does). With vcfg the radar also reports vitals, about one
// tick in eight, mostly in the plausible band when one person is in, the
// band's edges and beyond it otherwise, and the lock's two windows join the
// edges. Times stay far below 2^31, where the firmware's int32 elapsed()
// and the ports' subtraction agree.
function fuzz(cfg, seed, n, vcfg) {
  const rand = mulberry32(seed);
  const pick = (k) => Math.floor(rand() * k);
  const windows = [cfg.present_debounce_ms, cfg.clear_timeout_ms, cfg.stall_timeout_ms];
  if (vcfg) windows.push(vcfg.lock_confirm_ms, vcfg.lock_lost_ms);
  const band = (lo, hi) => [lo - 1, lo, lo + 1 + pick(hi - lo - 1), hi, hi + 1][pick(5)];
  const out = [{ reset: 0 }];
  let t = 0, here = 0;
  for (let i = 0; i < n; i++) {
    const r = rand();
    if (r < 0.04) t += windows[pick(windows.length)] + pick(3) - 1;
    else if (r < 0.05) t += cfg.stall_timeout_ms + pick(2000);
    else t += 10 + pick(140);
    if (rand() < 0.0005) { out.push({ reset: t }); continue; }
    if (rand() < 0.02) here = pick(4);
    if (vcfg && rand() < 0.125) {
      const sane = here === 1 && rand() < 0.85;
      out.push({ t, breath: sane ? 12 + pick(8) : band(vcfg.breath_min_bpm, vcfg.breath_max_bpm),
        heart: sane ? 60 + pick(30) : band(vcfg.heart_min_bpm, vcfg.heart_max_bpm) });
      continue;
    }
    const k = rand();
    if (k < 0.3) out.push({ t });                                              // a loop with no frame
    else if (here === 0) {
      if (k < 0.33) out.push({ t, target: false, count: 1 + pick(2), cm: 0 });  // a count beside a cleared flag
      else if (k < 0.36) out.push({ t, target: true, count: pick(2), cm: 50 + pick(550) }); // a flicker
      else out.push({ t, target: false, count: 0, cm: 0 });
    } else if (k < 0.35) out.push({ t, target: false, count: 0, cm: 0 });     // a one-frame dropout
    else if (k < 0.45) out.push({ t, target: true, count: 0, cm: 50 + pick(550) }); // the count frame still to land
    else out.push({ t, target: true, count: here, cm: 50 + pick(550) });
  }
  return out;
}

// The wellbeing build's traces (A107): a room read as the radar reports it,
// presence frames at 10 Hz and a vitals frame each second, each trace
// holding the steps sense-sim.js's vitals side was decided on (F131).
function vitalsTraces(cfg, vcfg) {
  const D = cfg.present_debounce_ms, C = cfg.clear_timeout_ms, S = cfg.stall_timeout_ms;
  const L = vcfg.lock_confirm_ms, X = vcfg.lock_lost_ms;
  const on = (t, count = 1, cm = 90) => ({ t, target: true, count, cm });
  const off = (t) => ({ t, target: false, count: 0, cm: 0 });
  const vit = (t, breath = 14, heart = 68) => ({ t, breath, heart });
  // presence frames from..to every 100 ms, a vitals frame on each whole
  // second, mk(t) the presence frame at t
  const room = (from, to, mk, v = vit) => {
    const out = [];
    for (let t = from; t < to; t += 100) out.push(t % 1000 === 400 ? v(t) : mk(t));
    return out;
  };
  return {
    // vitals frames during the Present debounce earn nothing (the count is
    // 0 until Present); the lock confirms on a run that starts after it; a
    // one-frame dropout, its count-0 return frame and a vitals frame
    // between them hold bpm_valid; the Clear tick cuts bpm_valid while the
    // lock rides out its window; the deadline then drops it to Lost
    lock: [{ reset: 0 }, off(10), on(1000, 0), on(1050, 1), vit(1100), on(1150, 1), vit(1200), on(1000 + D, 1),
      ...room(1400, 6000, on), off(6050), vit(6070), on(6100, 0), on(6150, 1), ...room(6200, 8000, on),
      ...room(8000, 8000 + C + 1000, off, off), ...room(9000 + C, 7400 + X + 600, off, off)],
    // a second person while the lock is still being earned restarts its
    // run even with no vitals frame between (the non-vitals tick's
    // ambiguity reset); held, the lock stops bpm_valid at once and resumes
    // it when the room is one again; vitals out of the plausible band (both
    // edges and past them) earn nothing, and the deadline ends the lock
    second: [{ reset: 0 }, off(10), ...room(1000, 1000 + D + 100, on, (t) => on(t)), ...room(1400, 3000, on),
      on(3000, 2), on(3100, 2), on(3200, 2), ...room(3300, 8000, on), on(8000, 2), on(8100, 2), vit(8400),
      on(8500, 2), ...room(8600, 10000, on), vit(10400, vcfg.breath_max_bpm + 1), vit(10500, vcfg.breath_max_bpm),
      vit(10600, 14, vcfg.heart_min_bpm - 1), vit(10700, vcfg.breath_min_bpm, vcfg.heart_max_bpm),
      vit(10800, 14, vcfg.heart_max_bpm + 1), ...room(11000, 18000, on, (t) => on(t))],
    // the radar goes silent while locked: the presence stall drops the
    // count, and bpm_valid with it, in its own tick, the lock falls to Lost
    // on its deadline, and the radar comes back to a person who earns both
    // afresh
    silence: [{ reset: 0 }, off(10), ...room(1000, 6000, on), { t: 6000 }, { t: 5900 + S },
      { t: 5400 + X - 1 }, { t: 5400 + X }, { t: 5400 + X + 10 }, ...room(5400 + X + 100, 5400 + X + 100 + D + L + 1500, on)],
    // a reset (reconfigure()) while locked: both FSMs start over
    reset: [{ reset: 0 }, off(10), ...room(1000, 7000, on), { reset: 7050 }, ...room(7100, 13000, on)],
    // the seeded random walk again, with vitals frames (the review's lens)
    fuzz: fuzz(cfg, 0x71a175, 10000, vcfg),
  };
}

module.exports = { MAIN_GATES, VITALS_GATES, build, runFirmware, traces, vitalsTraces, fuzz };
