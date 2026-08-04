// canary-local/assets/mic-sim.js — DOM-free core for the 4.3C dash microphone.
//
// This is the honesty layer for the mic-bearing dash (Waveshare 4.3C,
// docs/hardware/display_mic_variant.md): a faithful JavaScript port of the
// firmware's pure decision core, so a browser can run the SAME loud/quiet
// reasoning the device runs — from one loudness number per frame — without
// hardware and without pretending to be it.
//
//   Sensitivity presets      port firmware/projects/canary-display/
//   Envelope (adaptive floor)  include/canary/io/mic_logic.h
//   CadenceDetector (T3/T4)    (the pure, host-tested core — no I2S, no DOM)
//   TransientDetector (wake)   ES7210 capture + NVS live in the firmware only.
//   Gate (the listening latch)
//
// The port is arithmetic-for-arithmetic where it matters: the same integer
// shifts, the same percent-of-floor thresholds, the same disjoint T3/T4
// duration windows, the same two-cycle rule, the same refractory-gated
// wake-on-sound. tests/mic.test.js re-pins every constant against the
// firmware's own header AND replays the host test's scenarios through this
// port, so a drift in either side breaks CI.
//
// The privacy barrier is modeled honestly: this core takes an RMS SCALAR per
// frame — never samples — exactly as the runtime does after it averages each
// 20 ms buffer to one number and zeroes it. Nothing that could carry speech
// ever reaches here, in the firmware or in this file.
//
// No DOM, no fetch, no Date.now() — every method takes explicit time so Node
// tests (and the browser) drive it deterministically.

// ── integer helpers (mirror the firmware's uint16/uint32 math) ──────────────
const u16 = (v) => v & 0xffff;
const u32 = (v) => v >>> 0;
const idiv = (a, b) => Math.floor(a / b); // C integer division, a,b >= 0
const clamp16 = (v) => (v > 0xffff ? 0xffff : v);

// ── Sensitivity presets (mirror mic::SENS_* in mic_logic.h) ─────────────────
// {floor_min, on_pct, off_pct, floor_shift}. on = floor*on_pct/100 (dB over
// the tracked floor), off = floor*off_pct/100 (hysteresis, always < on).
export const SENS = {
  // Bedroom / nightstand — sensitivity first. +9 dB on / +5 dB off.
  quiet:    { floor_min: 100, on_pct: 282, off_pct: 178, floor_shift: 6 },
  // Living areas — the balanced default. +10 dB on / +6 dB off.
  standard: { floor_min: 180, on_pct: 316, off_pct: 200, floor_shift: 6 },
  // Kitchen / workshop — specificity first. +13 dB on / +8 dB off.
  noisy:    { floor_min: 350, on_pct: 450, off_pct: 251, floor_shift: 7 },
};
export const SENS_ORDER = ["quiet", "standard", "noisy"];
export const SENS_DEFAULT_INDEX = 1; // standard

export function sensByIndex(i) {
  return SENS[SENS_ORDER[i]] || SENS.standard;
}
export function sensName(i) {
  return SENS_ORDER[i] || "standard";
}
// Loudness-margin of a percent-of-floor threshold, in dB (for display copy).
export const dbOverFloor = (pct) => 20 * Math.log10(pct / 100);

// ── Envelope with an adaptive noise floor (mirror mic::Envelope) ────────────
export const FALL_SHIFT = 3; // floor falls ~8x faster than it rises

export class Envelope {
  constructor(profile) {
    this.s = profile || SENS.standard;
    this.floor = 0;
    this.loud = false;
    this.seeded = false;
  }
  setProfile(ns) {
    this.s = ns;
    this.floor = ns.floor_min;
    this.loud = false;
    this.seeded = true;
  }
  noiseFloor() {
    return this.floor < this.s.floor_min ? this.s.floor_min : this.floor;
  }
  onThreshold() {
    return clamp16(idiv(this.noiseFloor() * this.s.on_pct, 100));
  }
  offThreshold() {
    return clamp16(idiv(this.noiseFloor() * this.s.off_pct, 100));
  }
  // RMS scalar in, loud/quiet out (the hysteresis-clean flag the cadence
  // detector reads). Asymmetric follower: rise slow, fall fast.
  update(rms) {
    if (!this.seeded) {
      this.floor = this.s.floor_min;
      this.seeded = true;
    }
    if (rms >= this.floor) {
      this.floor = u16(this.floor + ((rms - this.floor) >>> this.s.floor_shift));
    } else {
      this.floor = u16(this.floor - ((this.floor - rms) >>> FALL_SHIFT));
    }
    if (this.floor < this.s.floor_min) this.floor = this.s.floor_min;
    const fl = this.floor; // clamped above == noiseFloor()
    if (!this.loud && rms >= idiv(fl * this.s.on_pct, 100)) this.loud = true;
    else if (this.loud && rms < idiv(fl * this.s.off_pct, 100)) this.loud = false;
    return this.loud;
  }
}

// ── Alarm-cadence detection (mirror mic::CadenceDetector) ────────────────────
// T3 (smoke, NFPA-72/ISO-8201): 3 beeps. T4 (CO, UL-2034): 4 beeps. Two
// consecutive on-grammar groups raise the event; confidence grows with cycles.
export const CADENCE = {
  T3_BEEP_MIN: 350, T3_BEEP_MAX: 800,
  T4_BEEP_MIN: 60,  T4_BEEP_MAX: 300,
  GROUP_GAP_MS: 900,
  RESET_GAP_MS: 12000,
};
export const EVENT_WIRE = {
  smoke_t3: "acoustic_smoke_alarm",
  co_t4: "acoustic_co_alarm",
  none: "",
};
export function eventWireName(e) {
  return EVENT_WIRE[e] || "";
}

export class CadenceDetector {
  constructor() {
    this.prev_loud = false;
    this.edge_ms = 0;
    this.beeps = 0;
    this.beep_min = 0xffff;
    this.beep_max = 0;
    this.streak_event = "none";
    this.streak = 0;
  }
  // Feed the hysteresis-clean loud flag with the frame time. Returns a
  // detection {event, cycles, confidence} when a group closes and extends a
  // streak to >= 2 cycles; event "none" otherwise.
  update(loud, now) {
    const out = { event: "none", cycles: 0, confidence: 0 };
    if (loud !== this.prev_loud) {
      if (loud) {
        // Rising edge after a long-dead room: a stale streak lends no credit.
        if (u32(now - this.edge_ms) >= CADENCE.RESET_GAP_MS) {
          this.streak = 0;
          this.streak_event = "none";
        }
      } else {
        // Falling edge: a beep just ended — measure it.
        const dur32 = u32(now - this.edge_ms);
        const dur = dur32 > 0xffff ? 0xffff : dur32;
        if (this.beeps < 255) this.beeps++;
        if (dur < this.beep_min) this.beep_min = dur;
        if (dur > this.beep_max) this.beep_max = dur;
      }
      this.prev_loud = loud;
      this.edge_ms = now;
      return out;
    }
    if (!loud && this.beeps > 0) {
      const quiet = u32(now - this.edge_ms);
      if (quiet >= CADENCE.RESET_GAP_MS) {
        this.beeps = 0;
        this.beep_min = 0xffff;
        this.beep_max = 0;
        this.streak = 0;
        this.streak_event = "none";
      } else if (quiet >= CADENCE.GROUP_GAP_MS) {
        // Group closed: classify by count + duration window.
        let e = "none";
        if (this.beeps === 3 && this.beep_min >= CADENCE.T3_BEEP_MIN &&
            this.beep_max <= CADENCE.T3_BEEP_MAX) {
          e = "smoke_t3";
        } else if (this.beeps === 4 && this.beep_min >= CADENCE.T4_BEEP_MIN &&
                   this.beep_max <= CADENCE.T4_BEEP_MAX) {
          e = "co_t4";
        }
        if (e !== "none" && e === this.streak_event) {
          if (this.streak < 255) this.streak++;
        } else {
          this.streak_event = e;
          this.streak = e === "none" ? 0 : 1;
        }
        this.beeps = 0;
        this.beep_min = 0xffff;
        this.beep_max = 0;
        if (this.streak >= 2) {
          out.event = this.streak_event;
          out.cycles = this.streak;
          const c = 50 + 15 * this.streak;
          out.confidence = c > 95 ? 95 : c;
        }
      }
    }
    return out;
  }
}

// ── Loud-onset / wake-on-sound (mirror mic::TransientDetector) ───────────────
// Fires once on the rising crossing of floor*rise_pct/100 — a real thump, well
// above the +10 dB an alarm beep clears — refractory-gated so one door-close is
// one wake, seeded on the first quiet frame so the capture-start glitch can't
// fire it. Envelope-only: it never learns WHAT the sound was.
export const TRANSIENT = { RISE_PCT: 600, REFRACTORY_MS: 4000 };

export class TransientDetector {
  constructor() {
    this.rise_pct = TRANSIENT.RISE_PCT;
    this.refractory_ms = TRANSIENT.REFRACTORY_MS;
    this.prev_over = false;
    this.seeded = false;
    this.has_fired = false;
    this.last_fire_ms = 0;
  }
  update(rms, floor, now) {
    const thresh = idiv(floor * this.rise_pct, 100);
    const over = rms >= thresh;
    let fired = false;
    if (over && !this.prev_over && this.seeded &&
        (!this.has_fired || u32(now - this.last_fire_ms) >= this.refractory_ms)) {
      fired = true;
      this.has_fired = true;
      this.last_fire_ms = now;
    }
    if (!over) this.seeded = true;
    this.prev_over = over;
    return fired;
  }
}

// ── The listening gate (mirror mic::Gate) ────────────────────────────────────
// One authority for "may the capture driver run". The indicator has no state
// of its own: indicatorLit() === running. OFF is the default; unset audio pins
// hard-block listening (armed alone is never enough).
export class Gate {
  constructor() {
    this.armed = false;
    this.pins_ok = false;
    this.running = false;
  }
  update() {
    const want = this.armed && this.pins_ok;
    if (want && !this.running) {
      this.running = true;
      return "start";
    }
    if (!want && this.running) {
      this.running = false;
      return "stop"; // hard mute: the driver is UNINSTALLED
    }
    return "none";
  }
}
export const indicatorLit = (g) => g.running;

// ── The whole listening pipeline, one call per frame ─────────────────────────
// What the page drives: RMS in, {loud, floor, on, off, detection, wake} out.
// Sensitivity is the one tunable axis; wake-on-sound is opt-in.
export class MicSim {
  constructor(sensIndex = SENS_DEFAULT_INDEX) {
    this.env = new Envelope();
    this.cad = new CadenceDetector();
    this.trans = new TransientDetector();
    this.wakeOnSound = false;
    this.setSensitivity(sensIndex);
  }
  setSensitivity(i) {
    this.sensIndex = i;
    this.env.setProfile(sensByIndex(i));
  }
  reset() {
    this.env.setProfile(sensByIndex(this.sensIndex));
    this.cad = new CadenceDetector();
    this.trans = new TransientDetector();
  }
  // rms: the frame's loudness scalar (0..65535). now: frame time in ms.
  feed(rms, now) {
    const loud = this.env.update(rms);
    const floor = this.env.noiseFloor();
    const detection = this.cad.update(loud, now);
    const wake = this.wakeOnSound
      ? this.trans.update(rms, floor, now)
      : false;
    return {
      rms,
      loud,
      floor,
      on: this.env.onThreshold(),
      off: this.env.offThreshold(),
      detection,
      wake,
    };
  }
}

// ── Reassurance: what the mic does, and what it does NOT do ──────────────────
// The single source for the "is / is not" copy. The page renders these and a
// live facts table built from the constants above, so both the words and the
// numbers a visitor reads come straight from the drift-locked core.
export const MIC_FACTS = {
  does: [
    "Listens for the two regulated alarm cadences — smoke (NFPA-72 T3, three beeps) and CO (UL-2034 T4, four beeps) — and raises an Alert only after two on-grammar cycles.",
    "With wake-on-sound opted in, lights the screen on a sudden loud onset (a door closing, a knock) so a dark wall dash shows the house the moment you walk in.",
    "Turns each 20 ms of sound into ONE loudness number and decides from the rhythm of loud/quiet edges, adapting to your room's own noise floor.",
    "Shows an amber ● MIC dot whenever — and only whenever — the capture driver is running. The dot IS the driver's on-bit; there is no second flag to desync.",
  ],
  doesNot: [
    "Never records, stores, buffers to disk, or transmits audio. Each frame is averaged to one number and the sample buffer is zeroed the same instant.",
    "Never recognizes speech or words. Loud/quiet edges carry no content — a conversation cannot be reconstructed from what crosses the barrier, by construction.",
    "Never listens by default. It is OFF until you arm it, and disarming UNINSTALLS the capture driver (a hard mute, not a muted flag).",
    "Never listens in the bench, demo, debug, or arcade gears — a quiet console with no MIC dot is the proof.",
  ],
  // A one-line answer to the question the usability protocol asks out loud.
  couldSomeoneHearYou:
    "No. The only thing that ever leaves the microphone is a loudness number every 20 ms. There is no audio to hear, keep, or send — the words never exist past the barrier.",
};

// The devices are a fleet — no bird-group word appears anywhere in this file.
