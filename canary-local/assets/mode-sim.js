// canary-local/assets/mode-sim.js — DOM-free core for the display Mode system.
//
// This file is the Mode system's honesty layer: a faithful JavaScript port of
// the canary-display mode registry and demo storyline, so a browser can stage
// the SAME boot-gear decisions and the SAME scripted household the firmware
// runs — without hardware and without pretending to be it.
//
//   registry (tokens/policy/boot)  ports firmware/projects/canary-display/
//                                  include/canary/mode/mode_registry.h
//   demo cast + beats + stepping   ports .../include/canary/mode/demo_script.h
//   latch semantics (enter/exit)   ports .../src/mode/mode_glue.cpp
//
// The port is line-for-line where it matters: an unknown or uncarried token
// fails safe to the fleet face, the legacy devmode bool still lands in the
// bench (a written token outranks it), only fleet takes OTA or arms the
// watchdog, every non-fleet gear is exit-by-long-press and bannered, and the
// demo loop resolves to all-quiet before it wraps. tests/mode.test.js re-pins
// all of this against the firmware's own source, so a drift in either side
// breaks CI.
//
// No DOM, no fetch, no Date.now() — every method takes explicit time so Node
// tests (and the browser) drive it deterministically.

// ── Registry: tokens + policy (mirrors mode_registry.h) ─────────────────────

export const MODE_TOKENS = ["fleet", "bench", "demo", "debug", "arcade"];

// One row per gear: {network, ota, watchdog, touchExit, banner} — the same
// five booleans, in the same order, as the firmware's ModePolicy rows.
export const MODES = {
  fleet:  { network: true,  ota: true,  watchdog: true,  touchExit: false, banner: false },
  bench:  { network: false, ota: false, watchdog: false, touchExit: true,  banner: true },
  demo:   { network: false, ota: false, watchdog: false, touchExit: true,  banner: true },
  debug:  { network: true,  ota: false, watchdog: false, touchExit: true,  banner: true },
  arcade: { network: false, ota: false, watchdog: false, touchExit: true,  banner: true },
};

export function modeFromToken(s) {
  return MODE_TOKENS.includes(s) ? s : "fleet"; // unknown/corrupt -> the product face
}

// caps: {dedicatedBench, hasDevmode, hasDemo, hasDebug, hasArcade}
export function modeAllowed(caps, token) {
  switch (token) {
    case "fleet":  return true; // always — it is the fail-safe
    case "bench":  return !!(caps.dedicatedBench || caps.hasDevmode);
    case "demo":   return !!caps.hasDemo;
    case "debug":  return !!caps.hasDebug;
    case "arcade": return !!caps.hasArcade;
    default:       return false;
  }
}

export function resolveBootMode(caps, storedToken, legacyDevmode) {
  if (caps.dedicatedBench) return "bench"; // the flash IS the mode choice
  const hasToken = !!(storedToken && storedToken.length);
  let want = modeFromToken(storedToken || "");
  if (!hasToken && legacyDevmode) want = "bench";
  return modeAllowed(caps, want) ? want : "fleet";
}

// ── Demo storyline (mirrors demo_script.h) ──────────────────────────────────

export const DEMO = {
  LOOP_S: 150,
  CAST: [
    { id: "demo-front-door", name: "Front Door", room: "entry", battery: 96 },
    { id: "demo-kitchen", name: "Kitchen", room: "kitchen", battery: 88 },
    { id: "demo-garage", name: "Garage", room: "garage", battery: 71 },
    { id: "demo-nursery", name: "Nursery", room: "nursery", battery: 93 },
  ],
  // intent carries the severity the story MEANS; the firmware host test pins
  // each one to the REAL classifier (classify_event), so this carried table
  // can never quietly disagree with the product vocabulary.
  BEATS: [
    { atS: 5, witness: 1, event: "boot", intent: "ok" },
    { atS: 15, witness: 1, event: "presence_detected", intent: "notice" },
    { atS: 30, witness: 3, event: "presence_detected", intent: "notice" },
    { atS: 45, witness: 0, event: "doorbell", intent: "warn" },
    { atS: 60, witness: 0, event: "cleared", intent: "ok" },
    { atS: 75, witness: 2, event: "after_hours_motion", intent: "warn" },
    { atS: 90, witness: 2, event: "glass_break", intent: "alert" },
    { atS: 110, witness: 2, event: "tamper_contact", intent: "tamper" },
    { atS: 135, witness: 2, event: "cleared", intent: "ok" },
  ],
};

// Which beats fire in the half-open window (prevS, nowS] on the looping
// clock? prevS === nowS means no time passed (never "a whole lap"); a lap
// takes a wrap (prevS > nowS). Returns beat indices, timeline order.
export function beatsBetween(prevS, nowS) {
  const out = [];
  for (let i = 0; i < DEMO.BEATS.length; i++) {
    const at = DEMO.BEATS[i].atS;
    let fire;
    if (prevS === nowS) fire = false;
    else if (prevS < nowS) fire = at > prevS && at <= nowS;
    else fire = at > prevS || at <= nowS; // wrapped
    if (fire) out.push(i);
  }
  return out;
}

// ── The latch, staged (mirrors mode_glue.cpp) ───────────────────────────────
//
// A tiny NVS stand-in: `nvs.mode` is the token, `nvs.devmode` the legacy
// bool. enter()/exitToFleet() write exactly what the firmware writes; boot()
// resolves exactly what the firmware boots.

export class ModeSim {
  constructor(caps) {
    this.caps = caps || {};
    this.nvs = {}; // {mode?, devmode?}
    this.active = "fleet";
    this.log = [];
  }
  _emit(line) {
    this.log.push(line);
    return line;
  }
  // Settings doorway: latch + "reboot". Bench also writes the legacy bool so
  // a firmware downgrade still lands in the bench the user asked for.
  enter(token) {
    if (!modeAllowed(this.caps, token) || token === "fleet") return this.active;
    this.nvs.mode = token;
    if (token === "bench") this.nvs.devmode = true;
    else delete this.nvs.devmode;
    this._emit(`MD1 latch mode=${token}`);
    return this.boot();
  }
  // Every gear's 3 s long-press: clear both latches, back to the product face.
  exitToFleet() {
    delete this.nvs.mode;
    delete this.nvs.devmode;
    this._emit("MD1 latch cleared");
    return this.boot();
  }
  boot() {
    this.active = resolveBootMode(this.caps, this.nvs.mode || "",
                                  !!this.nvs.devmode);
    this._emit(`MD1 boot mode=${this.active}`);
    return this.active;
  }
  // Demo playback: advance the loop clock, emit the beats that fired (the
  // same DM1 grammar the firmware prints). Only meaningful in demo.
  demoTick(prevS, nowS) {
    const fired = beatsBetween(prevS % DEMO.LOOP_S, nowS % DEMO.LOOP_S);
    return fired.map((i) => {
      const b = DEMO.BEATS[i];
      return this._emit(
        `DM1 ${nowS * 1000} EVT ${DEMO.CAST[b.witness].id} ${b.event}`);
    });
  }
}
