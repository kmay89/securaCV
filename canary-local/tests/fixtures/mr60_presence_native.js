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

// argv: present_debounce_ms clear_timeout_ms stall_timeout_ms near_cm mid_cm.
// stdin, one tick per line: "R <t>" (reset), "N <t>" (no frame this loop),
// "P <t> <has_target 0|1> <target_count> <distance_cm>" (a presence frame).
// stdout, one line per tick: "<state> <count> <range> <state_changed>
// <count_changed> <stalled> <signed events, comma-joined, or ->".
const DRIVER = String.raw`
#include "mr60_presence.h"
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
int main(int argc, char** argv) {
  if (argc != 6) return 2;
  PresenceConfig cfg;
  cfg.present_debounce_ms = (uint32_t)std::strtoul(argv[1], nullptr, 10);
  cfg.clear_timeout_ms    = (uint32_t)std::strtoul(argv[2], nullptr, 10);
  cfg.stall_timeout_ms    = (uint32_t)std::strtoul(argv[3], nullptr, 10);
  cfg.near_cm             = (uint16_t)std::strtoul(argv[4], nullptr, 10);
  cfg.mid_cm              = (uint16_t)std::strtoul(argv[5], nullptr, 10);
  PresenceFSM fsm(cfg);
  char op;
  unsigned long t;
  while (std::scanf(" %c %lu", &op, &t) == 2) {
    if (op == 'R') { fsm.reset((uint32_t)t); std::printf("reset\n"); continue; }
    Frame f;
    if (op == 'P') {
      unsigned has = 0, n = 0, cm = 0;
      if (std::scanf("%u %u %u", &has, &n, &cm) != 3) return 3;
      f.kind = FrameKind::Presence;
      f.has_target = has != 0;
      f.target_count = (uint8_t)n;
      f.distance_cm = (uint16_t)cm;
    } else if (op != 'N') {
      return 4;
    }
    const PresenceEvent ev = fsm.tick(f, (uint32_t)t);
    std::string signed_events;
    if (ev.state_changed) {
      if (ev.state == Presence::Present) signed_events += "presence_detected,";
      else if (ev.state == Presence::Clear) signed_events += "presence_cleared,";
    }
    if (ev.count_changed && fsm.state() == Presence::Present) signed_events += "occupancy_changed,";
    if (signed_events.empty()) signed_events = "-";
    else signed_events.pop_back();
    std::printf("%s %s %s %d %d %d %s\n", st(fsm.state()), ct(fsm.count()), rg(fsm.range()),
                ev.state_changed ? 1 : 0, ev.count_changed ? 1 : 0, ev.stalled ? 1 : 0,
                signed_events.c_str());
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
    const why = `no C++ compiler (${cxx}; set CXX) to build mr60_presence.cpp`;
    if (process.env.CI) throw new Error(`${why}, and CI has one: refusing to skip`);
    built = { skip: why };
    return built;
  }
  const dir = fs.mkdtempSync(join(os.tmpdir(), "securacv-mr60-presence-"));
  process.once("exit", () => fs.rmSync(dir, { recursive: true, force: true }));
  fs.writeFileSync(join(dir, "driver.cpp"), DRIVER);
  const bin = join(dir, "mr60_presence_driver");
  const args = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", MR60,
    join(dir, "driver.cpp"), join(MR60, "mr60_presence.cpp"), "-o", bin];
  const r = spawnSync(cxx, args, { encoding: "utf8" });
  if (r.status !== 0) throw new Error(`${cxx} could not build mr60_presence.cpp:\n${cxx} ${args.join(" ")}\n${r.stderr}`);
  built = { bin };
  return built;
}

// A trace is a list of ticks: { reset: t }, { t } (no frame) or
// { t, target, count, cm } (a presence frame: has_target, target_count,
// distance_cm). cfg is the firmware's PresenceConfig by its field names.
// Returns one row per tick that is not a reset.
function runFirmware(cfg, trace) {
  const b = build();
  if (b.skip) throw new Error(b.skip);
  const lines = trace.map((k) => ("reset" in k ? `R ${k.reset}`
    : "target" in k ? `P ${k.t} ${k.target ? 1 : 0} ${k.count} ${k.cm}` : `N ${k.t}`));
  const r = spawnSync(b.bin, [cfg.present_debounce_ms, cfg.clear_timeout_ms, cfg.stall_timeout_ms,
    cfg.near_cm, cfg.mid_cm].map(String), { input: lines.join("\n") + "\n", encoding: "utf8" });
  if (r.status !== 0) throw new Error(`the mr60_presence driver exited ${r.status}: ${r.stderr}`);
  return r.stdout.trim().split("\n").filter((l) => l !== "reset").map((l) => {
    const [state, count, range, sc, cc, stalled, events] = l.split(" ");
    return { state, count, range, state_changed: sc === "1", count_changed: cc === "1",
      stalled: stalled === "1", events: events === "-" ? [] : events.split(",") };
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
  };
}

module.exports = { MAIN_GATES, build, runFirmware, traces };
