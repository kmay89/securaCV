// canary-local/assets/radar-emu.js — the Sense device twin, DOM-free.
//
// The missing half of "emulate it completely": sense-sim.js already ports the
// radar pipeline (bytes → parser → FSMs → chokepoint); this module ports the
// firmware's POST-FLASH VOICE — the USB tuning console and the console lines
// main.cpp prints — and wires both onto that pipeline, so a page can speak to
// an emulated Sense exactly as it speaks to a flashed one over Web Serial.
//
//   EmuTuningConsole  ports firmware/common/console/tuning_console.h
//                     (command grammar, clamping, [tune]/[cfg] reply shapes —
//                     the behaviors tests_host/test_tuning_console.cpp pins)
//   EmuSense          ports the console-facing half of canary-sense main.cpp
//                     (SENSE_KNOBS table, tuning_stream_tick's [radar] line,
//                     drive_fsms' [sense]/[presence]/[vitals] lines)
//
// The dialect gate: everything EmuSense prints must round-trip through the
// REAL bench parsers (flash-core.js parseSenseLine/parseCfgLine/parseTuneLine)
// — tests/radar_dev.test.js enforces it, so the twin can never drift into a
// dialect the live bench wouldn't understand, or vice versa.
//
// No DOM, no fetch, no Date.now() — callers inject time and randomness.

import {
  FrameKind, Presence, CountBucket, VitalsLock,
  SensePipeline, sceneFrames,
} from "./sense-sim.js";

// ============================================================================
// Knob tables — SENSE_KNOBS[] from canary-sense main.cpp, as data.
// Bounds mirror include/canary/sense_config.h (SENSE_*_LO/HI); defaults mirror
// configs/canary-sense/{default,wellbeing}/config.h (CS_*). Drift-gated in
// tests/radar_dev.test.js against those headers.
// ============================================================================

export const PRESENCE_KNOBS = [
  { name: "debounce", unit: "ms",  help: "sustained target before 'present'", lo: 0,    hi: 3000,  def: 300 },
  { name: "clear",    unit: "ms",  help: "no target before 'clear'",          lo: 200,  hi: 10000, def: 1500 },
  { name: "stall",    unit: "ms",  help: "radar silence before 'unknown'",    lo: 1000, hi: 20000, def: 5000 },
  { name: "near",     unit: "cm",  help: "near band edge",                    lo: 50,   hi: 400,   def: 150 },
  { name: "mid",      unit: "cm",  help: "mid band edge (beyond = far)",      lo: 100,  hi: 600,   def: 350 },
];

export const VITALS_KNOBS = [
  { name: "vlock",      unit: "ms",  help: "sustained vitals before 'locked'", lo: 500,  hi: 15000, def: 4000 },
  { name: "vlost",      unit: "ms",  help: "no vitals before 'lost'",          lo: 1000, hi: 20000, def: 6000 },
  { name: "breath_min", unit: "bpm", help: "reject breathing below this",      lo: 3,    hi: 15,    def: 6 },
  { name: "breath_max", unit: "bpm", help: "reject breathing above this",      lo: 16,   hi: 60,    def: 30 },
  { name: "heart_min",  unit: "bpm", help: "reject heart rate below this",     lo: 30,   hi: 79,    def: 40 },
  { name: "heart_max",  unit: "bpm", help: "reject heart rate above this",     lo: 80,   hi: 220,   def: 130 },
];

export function knobTable(wellbeing) {
  return wellbeing ? [...PRESENCE_KNOBS, ...VITALS_KNOBS] : [...PRESENCE_KNOBS];
}

// ============================================================================
// EmuTuningConsole — JS port of securacv::console::TuningConsole
// ============================================================================
//
// knobs: [{ name, unit, help, lo, hi, def, get():u32, set(v):bool }] — the
// setter must clamp internally and return true only when the stored value
// actually changed (the sense_config posture), exactly like the C++ Knob.

export const MAX_LINE = 96;
export const STREAM_DEFAULT_MS = 1000;
export const STREAM_MIN_MS = 200;
export const STREAM_MAX_MS = 10000;

// Strict uint parse: digits only, fits in uint32. Rejects "", "12x", "-3".
function parseU32(s) {
  if (!s || !s.length) return null;
  let v = 0;
  for (const ch of s) {
    if (ch < "0" || ch > "9") return null;
    const d = ch.charCodeAt(0) - 48;
    if (v > (0xffffffff - d) / 10) return null; // overflow
    v = v * 10 + d;
  }
  return v;
}

export class EmuTuningConsole {
  constructor(knobs, writeLine) {
    this.knobs = knobs;
    this.write = writeLine;
    this.buf = "";
    this.overflow = false;
    this.streamMs = STREAM_DEFAULT_MS;
    this.raw = false;
    this.changed = false;
  }

  feed(text) {
    for (const c of String(text)) this._feedChar(c);
  }

  _feedChar(c) {
    if (c === "\r") return; // tolerate CRLF terminals
    if (c === "\n") {
      const line = this.buf;
      const wasOverflow = this.overflow;
      this.buf = "";
      this.overflow = false;
      if (wasOverflow) {
        this.write(`[tune] err line too long (max ${MAX_LINE - 1} chars)`);
        return;
      }
      this._execute(line);
      return;
    }
    if (this.buf.length + 1 >= MAX_LINE) { this.overflow = true; return; }
    this.buf += c;
  }

  streamPeriodMs() { return this.streamMs; }
  rawEnabled() { return this.raw; }
  takeChanged() { const c = this.changed; this.changed = false; return c; }

  printCfg() {
    let line = "[cfg]";
    for (const k of this.knobs) line += ` ${k.name}=${k.get()}`;
    line += ` stream=${this.streamMs} raw=${this.raw ? 1 : 0}`;
    this.write(line);
  }

  printHelp() {
    this.write("[tune] commands:");
    this.write("[tune]   help | ?              this list");
    this.write("[tune]   cfg                   all knobs on one line");
    this.write("[tune]   set <knob> <value>    change a knob (clamped, saved)");
    this.write("[tune]   reset                 restore compiled defaults");
    this.write("[tune]   stream on|off|<ms>    periodic [radar] line");
    this.write("[tune]   raw on|off            bench detail in the stream");
    this.write("[tune] knobs (name  range  current):");
    for (const k of this.knobs) {
      this.write(`[tune]   ${k.name.padEnd(11)} ${k.lo}..${k.hi} ${k.unit}  now ${k.get()}  (${k.help})`);
    }
  }

  _execute(line) {
    const tok = line.split(/[ \t]+/).filter(Boolean).slice(0, 3);
    if (tok.length === 0) return; // blank line — a terminal probing for life
    const cmd = tok[0].toLowerCase();

    if (cmd === "help" || cmd === "?") { this.printHelp(); return; }
    if (cmd === "cfg" || cmd === "get") { this.printCfg(); return; }

    if (cmd === "set") {
      if (tok.length < 3) { this.write("[tune] err usage: set <knob> <value>"); return; }
      const k = this.knobs.find((x) => x.name === tok[1].toLowerCase());
      if (!k) { this.write(`[tune] err unknown knob '${tok[1]}' (try 'help')`); return; }
      const asked = parseU32(tok[2]);
      if (asked == null) { this.write(`[tune] err '${tok[2]}' is not a whole number`); return; }
      let v = asked;
      if (v < k.lo) v = k.lo;
      if (v > k.hi) v = k.hi;
      const changed = k.set(v);
      if (changed) this.changed = true;
      if (asked !== v) {
        this.write(`[tune] ok ${k.name}=${v} (clamped from ${asked}; range ${k.lo}..${k.hi})`);
      } else {
        this.write(`[tune] ok ${k.name}=${v}${changed ? "" : " (unchanged)"}`);
      }
      this.printCfg();
      return;
    }

    if (cmd === "reset") {
      let any = false;
      for (const k of this.knobs) if (k.set(k.def)) any = true;
      if (any) this.changed = true;
      this.write(`[tune] ok defaults restored${any ? "" : " (already there)"}`);
      this.printCfg();
      return;
    }

    if (cmd === "stream") {
      if (tok.length < 2) { this.write("[tune] err usage: stream on|off|<ms>"); return; }
      const arg = tok[1].toLowerCase();
      if (arg === "off") {
        this.streamMs = 0;
        this.write("[tune] ok stream off");
        return;
      }
      let ms = STREAM_DEFAULT_MS;
      if (arg !== "on") {
        const v = parseU32(arg);
        if (v == null) { this.write("[tune] err usage: stream on|off|<ms>"); return; }
        ms = Math.min(Math.max(v, STREAM_MIN_MS), STREAM_MAX_MS);
      }
      this.streamMs = ms;
      this.write(`[tune] ok stream every ${ms} ms`);
      return;
    }

    if (cmd === "raw") {
      const arg = tok.length >= 2 ? tok[1].toLowerCase() : "";
      if (arg !== "on" && arg !== "off") { this.write("[tune] err usage: raw on|off"); return; }
      this.raw = arg === "on";
      this.write(`[tune] ok raw ${this.raw ? "on" : "off"}`);
      return;
    }

    this.write(`[tune] err unknown command '${tok[0]}' (try 'help')`);
  }
}

// ============================================================================
// EmuSense — the whole twin: knob store + console + pipeline + console lines
// ============================================================================
//
// The public shape deliberately mirrors what a Web Serial session gives the
// bench: `write(text)` is the host→device direction (the cable's writer),
// `onLine(line)` is device→host (the cable's reader, already line-split).
// Point the same bench UI at a serial port or at an EmuSense and the code
// downstream cannot tell the difference — which is the whole point.
//
// scene mirrors sense-sim.js sceneFrames(): { mount, mountHeight, tiltDeg,
// person:{x,y,posture,orientation,moving}, secondPerson, fan,
// truth:{breathBpm,heartBpm} } plus our `unplugged` switch (an unplugged
// radar is simply a scene that generates no bytes — the stall deadline is
// the firmware's job, and the twin runs the firmware's port of it).

export const TICK_MS = 100; // the bench cadence senselab.html established

export function defaultScene() {
  return {
    mount: "wall", mountHeight: 1.5, tiltDeg: 0,
    person: { x: 6.5, y: 0, posture: "standing", orientation: "facing", moving: false },
    secondPerson: false, fan: false, unplugged: false,
    truth: { breathBpm: 14, heartBpm: 72 },
  };
}

// mulberry32 — the deterministic PRNG the Sense Lab bench runs on.
export function mulberry32(seed) {
  let a = seed >>> 0;
  return function () {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const PRESENCE_STR = { [Presence.Unknown]: "unknown", [Presence.Clear]: "clear", [Presence.Present]: "present" };

export class EmuSense {
  // opts: { wellbeing, hw, seed, scene } — hw is devices/sense.json's
  // sandbox-physics block (the same one the Sense Lab passes to sceneFrames).
  constructor(opts) {
    this.wellbeing = !!opts.wellbeing;
    this.hw = opts.hw;
    this.scene = opts.scene || defaultScene();
    this.rand = mulberry32(opts.seed == null ? 0x5e42 : opts.seed);
    this.onLine = null;

    this.values = {};
    for (const k of knobTable(this.wellbeing)) this.values[k.name] = k.def;

    const say = (line) => { if (this.onLine) this.onLine(line); };
    this.console = new EmuTuningConsole(
      knobTable(this.wellbeing).map((k) => ({
        ...k,
        get: () => this.values[k.name],
        set: (v) => {
          const clamped = Math.min(Math.max(v, k.lo), k.hi);
          const changed = this.values[k.name] !== clamped;
          this.values[k.name] = clamped;
          return changed;
        },
      })),
      say,
    );
    this.say = say;

    this.pipeline = new SensePipeline(this._presenceCfg(), this._vitalsCfg(), this.wellbeing);
    this.lastRaw = { distance_cm: 0, target_count: 0, breath_rate: 0, heart_rate: 0 };
    this.pipeline.onFrame = (f) => {
      // g_last_raw: the latest parser aggregate, printed only under `raw on`.
      this.lastRaw = {
        distance_cm: f.distance_cm, target_count: f.target_count,
        breath_rate: f.breath_rate, heart_rate: f.heart_rate,
      };
    };

    this.lastStreamMs = 0;
    this.lastBpmLineMs = 0;
    // drive_fsms' change-gate statics start at Unknown/Zero/Unknown — no
    // line prints until the FSMs actually move off their boot state.
    this.lastSense = { state: Presence.Unknown, count: CountBucket.Zero, range: "unknown" };
    this.lastLock = this.wellbeing ? VitalsLock.Unknown : null;
    this.nowMs = 0;
  }

  _presenceCfg() {
    return {
      present_debounce_ms: this.values.debounce, clear_timeout_ms: this.values.clear,
      stall_timeout_ms: this.values.stall, near_cm: this.values.near, mid_cm: this.values.mid,
    };
  }
  _vitalsCfg() {
    if (!this.wellbeing) return null;
    return {
      lock_confirm_ms: this.values.vlock, lock_lost_ms: this.values.vlost,
      breath_min_bpm: this.values.breath_min, breath_max_bpm: this.values.breath_max,
      heart_min_bpm: this.values.heart_min, heart_max_bpm: this.values.heart_max,
    };
  }

  // Host → device (the cable's writer half).
  write(text) { this.console.feed(text); this._applyChanges(); }

  _applyChanges() {
    if (!this.console.takeChanged()) return;
    // reconfigure(): new numbers, same FSM state — mutate the cfg the FSMs
    // read (SensePipeline's constructors copied it, so write through).
    Object.assign(this.pipeline.presence.cfg, this._presenceCfg());
    if (this.pipeline.vitals) Object.assign(this.pipeline.vitals.cfg, this._vitalsCfg());
    // log_cfg_applied(): the [CFG] echo HA and the benches both watch for.
    let line = `[CFG] Radar reflexes: debounce=${this.values.debounce}ms clear=${this.values.clear}ms ` +
      `stall=${this.values.stall}ms near=${this.values.near}cm mid=${this.values.mid}cm`;
    if (this.wellbeing) {
      line += ` vlock=${this.values.vlock}ms vlost=${this.values.vlost}ms ` +
        `breath=${this.values.breath_min}..${this.values.breath_max}bpm ` +
        `heart=${this.values.heart_min}..${this.values.heart_max}bpm`;
    }
    this.say(line);
  }

  // One bench tick: generate the scene's wire bytes, run the firmware's
  // pipeline over them, print exactly the lines main.cpp would print.
  tick(nowMs) {
    this.nowMs = nowMs;
    if (!this.scene.unplugged) {
      const { bytes } = sceneFrames(this.scene, this.hw, TICK_MS, nowMs, this.rand);
      this.pipeline.feed(bytes);
    }
    const before = this.pipeline.events.length;
    this.pipeline.drive(nowMs);
    this._transitionLines(nowMs);
    this._streamTick(nowMs);
    return this.pipeline.events.slice(before).map((e) => e.name);
  }

  _transitionLines(now) {
    const s = this.pipeline.snapshot;
    const state = s.presence, count = s.count, range = s.range;
    const L = this.lastSense;
    if (state !== L.state || count !== L.count || range !== L.range) {
      // drive_fsms: the compact change-gated line the live bench reads.
      this.say(`[sense] ${state} count=${count} range=${range}`);
      if (state !== L.state) {
        // The FSM only ever re-enters Unknown via the stall deadline.
        const stalled = state === Presence.Unknown;
        this.say(`[presence] -> ${state}${stalled ? " (radar stall)" : ""}`);
      }
      this.lastSense = { state, count, range };
    }
    if (this.wellbeing) {
      const lock = this.pipeline.vitals.lock;
      if (lock !== this.lastLock && lock !== VitalsLock.Unknown) {
        this.say(`[vitals] breathing ${lock === VitalsLock.Locked ? "locked" : "lost"}`);
      }
      this.lastLock = lock;
      if (s.bpm_valid && now - this.lastBpmLineMs >= 1000) {
        this.lastBpmLineMs = now;
        this.say(`[vitals] breath=${s.breath_bpm} heart=${s.heart_bpm} bpm`);
      }
    }
  }

  _streamTick(now) {
    const period = this.console.streamPeriodMs();
    if (!period) return;
    if (now - this.lastStreamMs < period) return;
    this.lastStreamMs = now;
    const s = this.pipeline.snapshot;
    let line = `[radar] state=${PRESENCE_STR[s.presence] || s.presence} count=${s.count} range=${s.range}`;
    if (this.wellbeing) {
      const lock = this.pipeline.vitals.lock === VitalsLock.Locked ? "locked"
        : this.pipeline.vitals.lock === VitalsLock.Lost ? "lost" : "unknown";
      line += ` lock=${lock} breath=${s.breath_bpm} heart=${s.heart_bpm}`;
    }
    if (this.console.rawEnabled()) {
      line += ` raw_dist=${this.lastRaw.distance_cm}cm raw_count=${this.lastRaw.target_count}` +
        ` raw_breath=${this.lastRaw.breath_rate} raw_heart=${this.lastRaw.heart_rate}`;
    }
    line += ` errs=${this.pipeline.parser.errorCount()}`;
    this.say(line);
  }

  // Scene controls (the twin's "walk past it" levers).
  setScene(patch) {
    if (patch.person) Object.assign(this.scene.person, patch.person);
    if (patch.truth) Object.assign(this.scene.truth, patch.truth);
    for (const k of Object.keys(patch)) {
      if (k !== "person" && k !== "truth") this.scene[k] = patch[k];
    }
  }
}

export { FrameKind, Presence, CountBucket, VitalsLock };
